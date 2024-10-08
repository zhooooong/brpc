// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#include <google/protobuf/descriptor.h>         // MethodDescriptor
#include <google/protobuf/message.h>            // Message
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/io/coded_stream.h>
#include "butil/logging.h"                       // LOG()
#include "butil/time.h"
#include "butil/iobuf.h"                         // butil::IOBuf
#include "butil/raw_pack.h"                      // RawPacker RawUnpacker
#include "butil/memory/scope_guard.h"
#include "brpc/controller.h"                    // Controller
#include "brpc/socket.h"                        // Socket
#include "brpc/server.h"                        // Server
#include "brpc/span.h"
#include "brpc/compress.h"                      // ParseFromCompressedData
#include "brpc/stream_impl.h"
#include "brpc/rpc_dump.h"                      // SampledRequest
#include "brpc/rpc_pb_message_factory.h"
#include "brpc/policy/baidu_rpc_meta.pb.h"      // RpcRequestMeta
#include "brpc/policy/baidu_rpc_protocol.h"
#include "brpc/policy/most_common_message.h"
#include "brpc/policy/streaming_rpc_protocol.h"
#include "brpc/details/usercode_backup_pool.h"
#include "brpc/details/controller_private_accessor.h"
#include "brpc/details/server_private_accessor.h"

extern "C" {
void bthread_assign_data(void* data);
}


namespace brpc {
namespace policy {

DEFINE_bool(baidu_protocol_use_fullname, true,
            "If this flag is true, baidu_std puts service.full_name in requests"
            ", otherwise puts service.name (required by jprotobuf).");

DEFINE_bool(baidu_std_protocol_deliver_timeout_ms, false,
            "If this flag is true, baidu_std puts timeout_ms in requests.");

// Notes:
// 1. 12-byte header [PRPC][body_size][meta_size]
// 2. body_size and meta_size are in network byte order
// 3. Use service->full_name() + method_name to specify the method to call
// 4. `attachment_size' is set iff request/response has attachment
// 5. Not supported: chunk_info

// Pack header into `buf'
inline void PackRpcHeader(char* rpc_header, uint32_t meta_size, int payload_size) {
    uint32_t* dummy = (uint32_t*)rpc_header;  // suppress strict-alias warning
    *dummy = *(uint32_t*)"PRPC";
    butil::RawPacker(rpc_header + 4)
        .pack32(meta_size + payload_size)
        .pack32(meta_size);
}

static void SerializeRpcHeaderAndMeta(
    butil::IOBuf* out, const RpcMeta& meta, int payload_size) {
    const uint32_t meta_size = GetProtobufByteSize(meta);
    if (meta_size <= 244) { // most common cases
        char header_and_meta[12 + meta_size];
        PackRpcHeader(header_and_meta, meta_size, payload_size);
        ::google::protobuf::io::ArrayOutputStream arr_out(header_and_meta + 12, meta_size);
        ::google::protobuf::io::CodedOutputStream coded_out(&arr_out);
        meta.SerializeWithCachedSizes(&coded_out); // not calling ByteSize again
        CHECK(!coded_out.HadError());
        CHECK_EQ(0, out->append(header_and_meta, sizeof(header_and_meta)));
    } else {
        char header[12];
        PackRpcHeader(header, meta_size, payload_size);
        CHECK_EQ(0, out->append(header, sizeof(header)));
        butil::IOBufAsZeroCopyOutputStream buf_stream(out);
        ::google::protobuf::io::CodedOutputStream coded_out(&buf_stream);
        meta.SerializeWithCachedSizes(&coded_out);
        CHECK(!coded_out.HadError());
    }
}

ParseResult ParseRpcMessage(butil::IOBuf* source, Socket* socket,
                            bool /*read_eof*/, const void*) {
    char header_buf[12];
    const size_t n = source->copy_to(header_buf, sizeof(header_buf));
    if (n >= 4) {
        void* dummy = header_buf;
        if (*(const uint32_t*)dummy != *(const uint32_t*)"PRPC") {
            return MakeParseError(PARSE_ERROR_TRY_OTHERS);
        }
    } else {
        if (memcmp(header_buf, "PRPC", n) != 0) {
            return MakeParseError(PARSE_ERROR_TRY_OTHERS);
        }
    }
    if (n < sizeof(header_buf)) {
        return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
    }
    uint32_t body_size;
    uint32_t meta_size;
    butil::RawUnpacker(header_buf + 4).unpack32(body_size).unpack32(meta_size);
    if (body_size > FLAGS_max_body_size) {
        // We need this log to report the body_size to give users some clues
        // which is not printed in InputMessenger.
        LOG(ERROR) << "body_size=" << body_size << " from "
                   << socket->remote_side() << " is too large";
        return MakeParseError(PARSE_ERROR_TOO_BIG_DATA);
    } else if (source->length() < sizeof(header_buf) + body_size) {
        return MakeParseError(PARSE_ERROR_NOT_ENOUGH_DATA);
    }
    if (meta_size > body_size) {
        LOG(ERROR) << "meta_size=" << meta_size << " is bigger than body_size="
                   << body_size;
        // Pop the message
        source->pop_front(sizeof(header_buf) + body_size);
        return MakeParseError(PARSE_ERROR_TRY_OTHERS);
    }
    source->pop_front(sizeof(header_buf));
    MostCommonMessage* msg = MostCommonMessage::Get();
    source->cutn(&msg->meta, meta_size);
    source->cutn(&msg->payload, body_size - meta_size);
    return MakeMessage(msg);
}

static bool SerializeResponse(const google::protobuf::Message& res,
                              Controller& cntl, CompressType compress_type,
                              butil::IOBuf& buf) {
    if (res.GetDescriptor() == SerializedResponse::descriptor()) {
        buf.swap(((SerializedResponse&)res).serialized_data());
        return true;
    }

    if (!res.IsInitialized()) {
        cntl.SetFailed(ERESPONSE,
                       "Missing required fields in response: %s",
                       res.InitializationErrorString().c_str());
        return false;
    } else if (!SerializeAsCompressedData(res, &buf, compress_type)) {
        cntl.SetFailed(ERESPONSE,
                       "Fail to serialize response, CompressType=%s",
                       CompressTypeToCStr(compress_type));
        return false;
    }
    return true;
}

namespace {
struct BaiduProxyPBMessages : public RpcPBMessages {
    static BaiduProxyPBMessages* Get() {
        return butil::get_object<BaiduProxyPBMessages>();
    }

    static void Return(BaiduProxyPBMessages* messages) {
        messages->Clear();
        butil::return_object(messages);
    }

    void Clear() {
        request.Clear();
        response.Clear();
    }

    ::google::protobuf::Message* Request() override { return &request; }
    ::google::protobuf::Message* Response() override { return &response; }

    SerializedRequest request;
    SerializedResponse response;
};
}

// Used by UT, can't be static.
void SendRpcResponse(int64_t correlation_id,
                     Controller* cntl,
                     RpcPBMessages* messages,
                     const Server* server,
                     MethodStatus* method_status,
                     int64_t received_us) {
    ControllerPrivateAccessor accessor(cntl);
    Span* span = accessor.span();
    if (span) {
        span->set_start_send_us(butil::cpuwide_time_us());
    }
    Socket* sock = accessor.get_sending_socket();

    std::unique_ptr<Controller, LogErrorTextAndDelete> recycle_cntl(cntl);
    ConcurrencyRemover concurrency_remover(method_status, cntl, received_us);

    auto messages_guard = butil::MakeScopeGuard([server, messages] {
        if (NULL == messages) {
            return;
        }
        if (NULL != server->options().baidu_master_service) {
            BaiduProxyPBMessages::Return(static_cast<BaiduProxyPBMessages*>(messages));
        } else {
            server->options().rpc_pb_message_factory->Return(messages);
        }
    });

    const google::protobuf::Message* req = NULL == messages ? NULL : messages->Request();
    const google::protobuf::Message* res = NULL == messages ? NULL : messages->Response();
    ClosureGuard guard(brpc::NewCallback(
        cntl, &Controller::CallAfterRpcResp, req, res));
    
    StreamId response_stream_id = accessor.response_stream();

    if (cntl->IsCloseConnection()) {
        StreamClose(response_stream_id);
        sock->SetFailed();
        return;
    }
    bool append_body = false;
    butil::IOBuf res_body;
    // `res' can be NULL here, in which case we don't serialize it
    // If user calls `SetFailed' on Controller, we don't serialize
    // response either
    CompressType compress_type = cntl->response_compress_type();
    if (res != NULL && !cntl->Failed()) {
        append_body = SerializeResponse(
            *res, *cntl, compress_type, res_body);
    }

    // Don't use res->ByteSize() since it may be compressed
    size_t res_size = 0;
    size_t attached_size = 0;
    if (append_body) {
        res_size = res_body.length();
        attached_size = cntl->response_attachment().length();
    }

    int error_code = cntl->ErrorCode();
    if (error_code == -1) {
        // replace general error (-1) with INTERNAL_SERVER_ERROR to make a
        // distinction between server error and client error
        error_code = EINTERNAL;
    }
    RpcMeta meta;
    RpcResponseMeta* response_meta = meta.mutable_response();
    response_meta->set_error_code(error_code);
    if (!cntl->ErrorText().empty()) {
        // Only set error_text when it's not empty since protobuf Message
        // always new the string no matter if it's empty or not.
        response_meta->set_error_text(cntl->ErrorText());
    }
    meta.set_correlation_id(correlation_id);
    meta.set_compress_type(compress_type);
    if (attached_size > 0) {
        meta.set_attachment_size(attached_size);
    }
    SocketUniquePtr stream_ptr;
    if (response_stream_id != INVALID_STREAM_ID) {
        if (Socket::Address(response_stream_id, &stream_ptr) == 0) {
            Stream* s = (Stream*)stream_ptr->conn();
            s->FillSettings(meta.mutable_stream_settings());
            s->SetHostSocket(sock);
        } else {
            LOG(WARNING) << "Stream=" << response_stream_id 
                         << " was closed before sending response";
        }
    }

    if (cntl->has_response_user_fields() &&
        !cntl->response_user_fields()->empty()) {
        ::google::protobuf::Map<std::string, std::string>& user_fields
            = *meta.mutable_user_fields();
        user_fields.insert(cntl->response_user_fields()->begin(),
                           cntl->response_user_fields()->end());

    }

    butil::IOBuf res_buf;
    SerializeRpcHeaderAndMeta(&res_buf, meta, res_size + attached_size);
    if (append_body) {
        res_buf.append(res_body.movable());
        if (attached_size > 0) {
            res_buf.append(cntl->response_attachment().movable());
        }
    }

    if (span) {
        span->set_response_size(res_buf.size());
    }
    // Send rpc response over stream even if server side failed to create
    // stream for some reason.
    if(cntl->has_remote_stream()){
        // Send the response over stream to notify that this stream connection
        // is successfully built.
        // Response_stream can be INVALID_STREAM_ID when error occurs.
        if (SendStreamData(sock, &res_buf,
                           accessor.remote_stream_settings()->stream_id(),
                           accessor.response_stream()) != 0) {
            const int errcode = errno;
            std::string error_text = butil::string_printf(64, "Fail to write into %s",
                                                          sock->description().c_str());
            PLOG_IF(WARNING, errcode != EPIPE) << error_text;
            cntl->SetFailed(errcode,  "%s", error_text.c_str());
            if(stream_ptr) {
                ((Stream*)stream_ptr->conn())->Close(errcode, "%s",
                                                     error_text.c_str());
            }
            return;
        }

        if(stream_ptr) {
            // Now it's ok the mark this server-side stream as connected as all the
            // written user data would follower the RPC response.
            ((Stream*)stream_ptr->conn())->SetConnected();
        }
    } else{
        // Have the risk of unlimited pending responses, in which case, tell
        // users to set max_concurrency.
        Socket::WriteOptions wopt;
        wopt.ignore_eovercrowded = true;
        if (sock->Write(&res_buf, &wopt) != 0) {
            const int errcode = errno;
            PLOG_IF(WARNING, errcode != EPIPE) << "Fail to write into " << *sock;
            cntl->SetFailed(errcode, "Fail to write into %s",
                            sock->description().c_str());
            return;
        }
    }

    if (span) {
        // TODO: this is not sent
        span->set_sent_us(butil::cpuwide_time_us());
    }
}

namespace {
struct CallMethodInBackupThreadArgs {
    ::google::protobuf::Service* service;
    const ::google::protobuf::MethodDescriptor* method;
    ::google::protobuf::RpcController* controller;
    const ::google::protobuf::Message* request;
    ::google::protobuf::Message* response;
    ::google::protobuf::Closure* done;
};
}

static void CallMethodInBackupThread(void* void_args) {
    CallMethodInBackupThreadArgs* args = (CallMethodInBackupThreadArgs*)void_args;
    args->service->CallMethod(args->method, args->controller, args->request,
                              args->response, args->done);
    delete args;
}

// Used by other protocols as well.
void EndRunningCallMethodInPool(
    ::google::protobuf::Service* service,
    const ::google::protobuf::MethodDescriptor* method,
    ::google::protobuf::RpcController* controller,
    const ::google::protobuf::Message* request,
    ::google::protobuf::Message* response,
    ::google::protobuf::Closure* done) {
    CallMethodInBackupThreadArgs* args = new CallMethodInBackupThreadArgs;
    args->service = service;
    args->method = method;
    args->controller = controller;
    args->request = request;
    args->response = response;
    args->done = done;
    return EndRunningUserCodeInPool(CallMethodInBackupThread, args);
};

void ProcessRpcRequest(InputMessageBase* msg_base) {
    const int64_t start_parse_us = butil::cpuwide_time_us();
    DestroyingPtr<MostCommonMessage> msg(static_cast<MostCommonMessage*>(msg_base));
    SocketUniquePtr socket_guard(msg->ReleaseSocket());
    Socket* socket = socket_guard.get();
    const Server* server = static_cast<const Server*>(msg_base->arg());
    ScopedNonServiceError non_service_error(server);

    RpcMeta meta;
    if (!ParsePbFromIOBuf(&meta, msg->meta)) {
        LOG(WARNING) << "Fail to parse RpcMeta from " << *socket;
        socket->SetFailed(EREQUEST, "Fail to parse RpcMeta from %s",
                          socket->description().c_str());
        return;
    }
    const RpcRequestMeta &request_meta = meta.request();

    SampledRequest* sample = AskToBeSampled();
    if (sample) {
        sample->meta.set_service_name(request_meta.service_name());
        sample->meta.set_method_name(request_meta.method_name());
        sample->meta.set_compress_type((CompressType)meta.compress_type());
        sample->meta.set_protocol_type(PROTOCOL_BAIDU_STD);
        sample->meta.set_attachment_size(meta.attachment_size());
        sample->meta.set_authentication_data(meta.authentication_data());
        sample->request = msg->payload;
        sample->submit(start_parse_us);
    }

    std::unique_ptr<Controller> cntl(new (std::nothrow) Controller);
    if (NULL == cntl.get()) {
        LOG(WARNING) << "Fail to new Controller";
        return;
    }

    RpcPBMessages* messages = NULL;

    ServerPrivateAccessor server_accessor(server);
    ControllerPrivateAccessor accessor(cntl.get());
    const bool security_mode = server->options().security_mode() &&
                               socket->user() == server_accessor.acceptor();
    if (request_meta.has_log_id()) {
        cntl->set_log_id(request_meta.log_id());
    }
    if (request_meta.has_request_id()) {
        cntl->set_request_id(request_meta.request_id());
    }
    if (request_meta.has_timeout_ms()) {
        cntl->set_timeout_ms(request_meta.timeout_ms());
    }
    cntl->set_request_compress_type((CompressType)meta.compress_type());
    accessor.set_server(server)
        .set_security_mode(security_mode)
        .set_peer_id(socket->id())
        .set_remote_side(socket->remote_side())
        .set_local_side(socket->local_side())
        .set_auth_context(socket->auth_context())
        .set_request_protocol(PROTOCOL_BAIDU_STD)
        .set_begin_time_us(msg->received_us())
        .move_in_server_receiving_sock(socket_guard);

    if (meta.has_stream_settings()) {
        accessor.set_remote_stream_settings(meta.release_stream_settings());
    }

    if (!meta.user_fields().empty()) {
        for (const auto& it : meta.user_fields()) {
            (*cntl->request_user_fields())[it.first] = it.second;
        }
    }

    // Tag the bthread with this server's key for thread_local_data().
    if (server->thread_local_options().thread_local_data_factory) {
        bthread_assign_data((void*)&server->thread_local_options());
    }

    Span* span = NULL;
    if (IsTraceable(request_meta.has_trace_id())) {
        span = Span::CreateServerSpan(
            request_meta.trace_id(), request_meta.span_id(),
            request_meta.parent_span_id(), msg->base_real_us());
        accessor.set_span(span);
        span->set_log_id(request_meta.log_id());
        span->set_remote_side(cntl->remote_side());
        span->set_protocol(PROTOCOL_BAIDU_STD);
        span->set_received_us(msg->received_us());
        span->set_start_parse_us(start_parse_us);
        span->set_request_size(msg->payload.size() + msg->meta.size() + 12);
    }

    MethodStatus* method_status = NULL;
    do {
        if (!server->IsRunning()) {
            cntl->SetFailed(ELOGOFF, "Server is stopping");
            break;
        }

        if (socket->is_overcrowded()) {
            cntl->SetFailed(EOVERCROWDED, "Connection to %s is overcrowded",
                            butil::endpoint2str(socket->remote_side()).c_str());
            break;
        }
        
        if (!server_accessor.AddConcurrency(cntl.get())) {
            cntl->SetFailed(
                ELIMIT, "Reached server's max_concurrency=%d",
                server->options().max_concurrency);
            break;
        }

        if (FLAGS_usercode_in_pthread && TooManyUserCode()) {
            cntl->SetFailed(ELIMIT, "Too many user code to run when"
                            " -usercode_in_pthread is on");
            break;
        }

        const int req_size = static_cast<int>(msg->payload.size());
        if (meta.has_attachment_size()) {
            if (req_size < meta.attachment_size()) {
                cntl->SetFailed(EREQUEST,
                    "attachment_size=%d is larger than request_size=%d",
                    meta.attachment_size(), req_size);
                break;
            }
        }

        google::protobuf::Service* svc = NULL;
        google::protobuf::MethodDescriptor* method = NULL;
        if (NULL != server->options().baidu_master_service) {
            svc = server->options().baidu_master_service;
            auto sampled_request = new (std::nothrow) SampledRequest;
            if (NULL == sampled_request) {
                cntl->SetFailed(ENOMEM, "Fail to get sampled_request");
                break;
            }
            sampled_request->meta.set_service_name(request_meta.service_name());
            sampled_request->meta.set_method_name(request_meta.method_name());
            cntl->reset_sampled_request(sampled_request);
            // Switch to service-specific error.
            non_service_error.release();
            method_status = server->options().baidu_master_service->_status;
            if (method_status) {
                int rejected_cc = 0;
                if (!method_status->OnRequested(&rejected_cc, cntl.get())) {
                    cntl->SetFailed(
                        ELIMIT,
                        "Rejected by %s's ConcurrencyLimiter, concurrency=%d",
                        butil::class_name<BaiduMasterService>(), rejected_cc);
                    break;
                }
            }
            if (span) {
                span->ResetServerSpanName(sampled_request->meta.method_name());
            }

            messages = BaiduProxyPBMessages::Get();
            msg->payload.cutn(
                &((SerializedRequest*)messages->Request())->serialized_data(),
                req_size - meta.attachment_size());
            if (!msg->payload.empty()) {
                cntl->request_attachment().swap(msg->payload);
            }
        } else {
            // NOTE(gejun): jprotobuf sends service names without packages. So the
            // name should be changed to full when it's not.
            butil::StringPiece svc_name(request_meta.service_name());
            if (svc_name.find('.') == butil::StringPiece::npos) {
                const Server::ServiceProperty* sp =
                    server_accessor.FindServicePropertyByName(svc_name);
                if (NULL == sp) {
                    cntl->SetFailed(ENOSERVICE, "Fail to find service=%s",
                        request_meta.service_name().c_str());
                    break;
                }
                svc_name = sp->service->GetDescriptor()->full_name();
            }
            const Server::MethodProperty* mp =
                server_accessor.FindMethodPropertyByFullName(
                    svc_name, request_meta.method_name());
            if (NULL == mp) {
                cntl->SetFailed(ENOMETHOD, "Fail to find method=%s/%s",
                                request_meta.service_name().c_str(),
                                request_meta.method_name().c_str());
                break;
            } else if (mp->service->GetDescriptor() == BadMethodService::descriptor()) {
                BadMethodRequest breq;
                BadMethodResponse bres;
                breq.set_service_name(request_meta.service_name());
                mp->service->CallMethod(mp->method, cntl.get(), &breq, &bres, NULL);
                break;
            }
            // Switch to service-specific error.
            non_service_error.release();
            method_status = mp->status;
            if (method_status) {
                int rejected_cc = 0;
                if (!method_status->OnRequested(&rejected_cc, cntl.get())) {
                    cntl->SetFailed(
                        ELIMIT,
                        "Rejected by %s's ConcurrencyLimiter, concurrency=%d",
                        mp->method->full_name().c_str(), rejected_cc);
                    break;
                }
            }
            svc = mp->service;
            method = const_cast<google::protobuf::MethodDescriptor*>(mp->method);
            accessor.set_method(method);

            if (span) {
                span->ResetServerSpanName(method->full_name());
            }

            if (!server->AcceptRequest(cntl.get())) {
                break;
            }

            butil::IOBuf req_buf;
            int body_without_attachment_size = req_size - meta.attachment_size();
            msg->payload.cutn(&req_buf, body_without_attachment_size);
            if (meta.attachment_size() > 0) {
                cntl->request_attachment().swap(msg->payload);
            }

            auto req_cmp_type = static_cast<CompressType>(meta.compress_type());
            messages = server->options().rpc_pb_message_factory->Get(*svc, *method);
            if (!ParseFromCompressedData(req_buf, messages->Request(), req_cmp_type)) {
                cntl->SetFailed(EREQUEST, "Fail to parse request message, "
                                          "CompressType=%s, request_size=%d",
                                CompressTypeToCStr(req_cmp_type), req_size);
                server->options().rpc_pb_message_factory->Return(messages);
                break;
            }
            req_buf.clear();
        }

        // `socket' will be held until response has been sent
        google::protobuf::Closure* done = ::brpc::NewCallback<
            int64_t, Controller*, RpcPBMessages*,
            const Server*, MethodStatus*, int64_t>(&SendRpcResponse,
                                                   meta.correlation_id(),
                                                   cntl.get(), messages,
                                                   server, method_status,
                                                   msg->received_us());

        // optional, just release resource ASAP
        msg.reset();

        if (span) {
            span->set_start_callback_us(butil::cpuwide_time_us());
            span->AsParent();
        }
        if (!FLAGS_usercode_in_pthread) {
            return svc->CallMethod(method, cntl.release(), 
                                   messages->Request(),
                                   messages->Response(), done);
        }
        if (BeginRunningUserCode()) {
            svc->CallMethod(method, cntl.release(), 
                            messages->Request(),
                            messages->Response(), done);
            return EndRunningUserCodeInPlace();
        } else {
            return EndRunningCallMethodInPool(
                svc, method, cntl.release(),
                messages->Request(),
                messages->Response(), done);
        }
    } while (false);
    
    // `cntl', `req' and `res' will be deleted inside `SendRpcResponse'
    // `socket' will be held until response has been sent
    SendRpcResponse(meta.correlation_id(),
                    cntl.release(), messages,
                    server, method_status,
                    msg->received_us());
}

bool VerifyRpcRequest(const InputMessageBase* msg_base) {
    const MostCommonMessage* msg =
        static_cast<const MostCommonMessage*>(msg_base);
    const Server* server = static_cast<const Server*>(msg->arg());
    Socket* socket = msg->socket();
    
    RpcMeta request_meta;
    if (!ParsePbFromIOBuf(&request_meta, msg->meta)) {
        LOG(WARNING) << "Fail to parse RpcRequestMeta";
        return false;
    }
    const Authenticator* auth = server->options().auth;
    if (NULL == auth) {
        // Fast pass (no authentication)
        return true;
    }
    if (auth->VerifyCredential(request_meta.authentication_data(),
                               socket->remote_side(),
                               socket->mutable_auth_context()) == 0) {
        return true;
    }

    // Send `ERPCAUTH' to client.
    RpcMeta response_meta;
    response_meta.set_correlation_id(request_meta.correlation_id());
    response_meta.mutable_response()->set_error_code(ERPCAUTH);
    response_meta.mutable_response()->set_error_text("Fail to authenticate");
    std::string user_error_text = auth->GetUnauthorizedErrorText();
    if (!user_error_text.empty()) {
        response_meta.mutable_response()->mutable_error_text()->append(": ");
        response_meta.mutable_response()->mutable_error_text()->append(user_error_text);
    }
    butil::IOBuf res_buf;
    SerializeRpcHeaderAndMeta(&res_buf, response_meta, 0);
    Socket::WriteOptions opt;
    opt.ignore_eovercrowded = true;
    if (socket->Write(&res_buf, &opt) != 0) {
        PLOG_IF(WARNING, errno != EPIPE) << "Fail to write into " << *socket;
    }

    return false;
}

void ProcessRpcResponse(InputMessageBase* msg_base) {
    const int64_t start_parse_us = butil::cpuwide_time_us();
    DestroyingPtr<MostCommonMessage> msg(static_cast<MostCommonMessage*>(msg_base));
    RpcMeta meta;
    if (!ParsePbFromIOBuf(&meta, msg->meta)) {
        LOG(WARNING) << "Fail to parse from response meta";
        return;
    }

    const bthread_id_t cid = { static_cast<uint64_t>(meta.correlation_id()) };
    Controller* cntl = NULL;

    StreamId remote_stream_id = meta.has_stream_settings() ? meta.stream_settings().stream_id(): INVALID_STREAM_ID;

    const int rc = bthread_id_lock(cid, (void**)&cntl);
    if (rc != 0) {
        LOG_IF(ERROR, rc != EINVAL && rc != EPERM)
            << "Fail to lock correlation_id=" << cid << ": " << berror(rc);
        if (remote_stream_id != INVALID_STREAM_ID) {
            SendStreamRst(msg->socket(), meta.stream_settings().stream_id());
        }
        return;
    }
    
    ControllerPrivateAccessor accessor(cntl);
    if (remote_stream_id != INVALID_STREAM_ID) {
        accessor.set_remote_stream_settings(
                new StreamSettings(meta.stream_settings()));
    }

    if (!meta.user_fields().empty()) {
        for (const auto& it : meta.user_fields()) {
            (*cntl->response_user_fields())[it.first] = it.second;
        }
    }

    Span* span = accessor.span();
    if (span) {
        span->set_base_real_us(msg->base_real_us());
        span->set_received_us(msg->received_us());
        span->set_response_size(msg->meta.size() + msg->payload.size() + 12);
        span->set_start_parse_us(start_parse_us);
    }
    const RpcResponseMeta &response_meta = meta.response();
    const int saved_error = cntl->ErrorCode();
    do {
        if (response_meta.error_code() != 0) {
            // If error_code is unset, default is 0 = success.
            cntl->SetFailed(response_meta.error_code(), 
                                  "%s", response_meta.error_text().c_str());
            break;
        } 
        // Parse response message iff error code from meta is 0
        butil::IOBuf res_buf;
        const int res_size = msg->payload.length();
        butil::IOBuf* res_buf_ptr = &msg->payload;
        if (meta.has_attachment_size()) {
            if (meta.attachment_size() > res_size) {
                cntl->SetFailed(
                    ERESPONSE,
                    "attachment_size=%d is larger than response_size=%d",
                    meta.attachment_size(), res_size);
                break;
            }
            int body_without_attachment_size = res_size - meta.attachment_size();
            msg->payload.cutn(&res_buf, body_without_attachment_size);
            res_buf_ptr = &res_buf;
            cntl->response_attachment().swap(msg->payload);
        }

        auto res_cmp_type = (CompressType)meta.compress_type();
        cntl->set_response_compress_type(res_cmp_type);
        if (cntl->response()) {
            if (cntl->response()->GetDescriptor() == SerializedResponse::descriptor()) {
                ((SerializedResponse*)cntl->response())->
                    serialized_data().append(*res_buf_ptr);
            } else if (!ParseFromCompressedData(
                    *res_buf_ptr, cntl->response(), res_cmp_type)) {
                cntl->SetFailed(
                    ERESPONSE, "Fail to parse response message, "
                    "CompressType=%s, response_size=%d", 
                    CompressTypeToCStr(res_cmp_type), res_size);
            }
        } // else silently ignore the response.        
    } while (0);
    // Unlocks correlation_id inside. Revert controller's
    // error code if it version check of `cid' fails
    msg.reset();  // optional, just release resource ASAP
    accessor.OnResponse(cid, saved_error);
}

void PackRpcRequest(butil::IOBuf* req_buf,
                    SocketMessage**,
                    uint64_t correlation_id,
                    const google::protobuf::MethodDescriptor* method,
                    Controller* cntl,
                    const butil::IOBuf& request_body,
                    const Authenticator* auth) {
    RpcMeta meta;
    if (auth && auth->GenerateCredential(
            meta.mutable_authentication_data()) != 0) {
        return cntl->SetFailed(EREQUEST, "Fail to generate credential");
    }

    ControllerPrivateAccessor accessor(cntl);
    RpcRequestMeta* request_meta = meta.mutable_request();
    if (method) {
        request_meta->set_service_name(FLAGS_baidu_protocol_use_fullname ?
                                       method->service()->full_name() :
                                       method->service()->name());
        request_meta->set_method_name(method->name());
        meta.set_compress_type(cntl->request_compress_type());
    } else if (NULL != cntl->sampled_request()) {
        // Replaying. Keep service-name as the one seen by server.
        request_meta->set_service_name(cntl->sampled_request()->meta.service_name());
        request_meta->set_method_name(cntl->sampled_request()->meta.method_name());
        meta.set_compress_type(cntl->sampled_request()->meta.has_compress_type() ?
                               cntl->sampled_request()->meta.compress_type() :
                               cntl->request_compress_type());
    } else {
        return cntl->SetFailed(ENOMETHOD, "%s.method is NULL", __func__ );
    }
    if (cntl->has_log_id()) {
        request_meta->set_log_id(cntl->log_id());
    }
    if (!cntl->request_id().empty()) {
        request_meta->set_request_id(cntl->request_id());
    }
    meta.set_correlation_id(correlation_id);
    StreamId request_stream_id = accessor.request_stream();
    if (request_stream_id != INVALID_STREAM_ID) {
        SocketUniquePtr ptr;
        if (Socket::Address(request_stream_id, &ptr) != 0) {
            return cntl->SetFailed(EREQUEST, "Stream=%" PRIu64 " was closed", 
                                   request_stream_id);
        }
        Stream *s = (Stream*)ptr->conn();
        s->FillSettings(meta.mutable_stream_settings());
    }

    if (cntl->has_request_user_fields() && !cntl->request_user_fields()->empty()) {
        ::google::protobuf::Map<std::string, std::string>& user_fields
            = *meta.mutable_user_fields();
        user_fields.insert(cntl->request_user_fields()->begin(),
                           cntl->request_user_fields()->end());
    }

    // Don't use res->ByteSize() since it may be compressed
    const size_t req_size = request_body.length(); 
    const size_t attached_size = cntl->request_attachment().length();
    if (attached_size) {
        meta.set_attachment_size(attached_size);
    }

    if (FLAGS_baidu_std_protocol_deliver_timeout_ms) {
        if (accessor.real_timeout_ms() > 0) {
            request_meta->set_timeout_ms(accessor.real_timeout_ms());
        }
    }

    Span* span = accessor.span();
    if (span) {
        request_meta->set_trace_id(span->trace_id());
        request_meta->set_span_id(span->span_id());
        request_meta->set_parent_span_id(span->parent_span_id());
    }

    SerializeRpcHeaderAndMeta(req_buf, meta, req_size + attached_size);
    req_buf->append(request_body);
    if (attached_size) {
        req_buf->append(cntl->request_attachment());
    }
}

}  // namespace policy
} // namespace brpc
