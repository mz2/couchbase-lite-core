//
//  CivetWebSocket.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/30/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "CivetWebSocket.hh"
#include "c4Socket+Internal.hh"
#include "c4Replicator.h"
#include "Actor.hh"
#include "Logging.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "civetweb.h"
#include <exception>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _MSC_VER
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#endif

using namespace std;
using namespace fleece;
using namespace fleeceapi;


namespace litecore { namespace websocket {


    static Address addressOf(struct mg_connection *connection) {
        auto info = mg_get_request_info(connection);
        return Address((info->is_ssl ? "blips" : "blip"),
                       info->remote_addr,
                       (uint16_t)info->remote_port);
    }


    static Address c4AddressOf(const C4Address &addr) {
        return websocket::Address(asstring(addr.scheme),
                                  asstring(addr.hostname),
                                  addr.port,
                                  asstring(addr.path));
    }
    
    
#pragma mark - WEBSOCKET CLASS


    class CivetWebSocket : public actor::Actor, public WebSocket {
    public:
        // Client-side constructor
        CivetWebSocket(Provider &provider,
                       const Address &to,
                       const AllocedDict &options)
        :WebSocket(provider, to)
        ,_connection(nullptr)
        ,_isServer(false)
        ,_options(options)
        { }


        // Server-side constructor: takes an already-open connection
        CivetWebSocket(Provider &provider,
                       struct mg_connection *connection)
        :WebSocket(provider, addressOf(connection))
        ,_connection(connection)
        ,_isServer(true)
        {
            mg_set_user_connection_data(connection, this);
        }


        virtual ~CivetWebSocket() {
            if (_connection)
                mg_close_connection(_connection);
        }


        virtual void connect() override {
            enqueue(&CivetWebSocket::_connect);
        }


        void _connect() {
            Assert(!_connection);

            stringstream extraHeaders;
            for (Dict::iterator header(_options[kC4ReplicatorOptionExtraHeaders].asDict());
                     header; ++header) {
                extraHeaders << slice(header.keyString()).asString() << ": "
                             << header.value().asstring() << "\r\n";
            }
            slice cookies = _options[kC4ReplicatorOptionCookies].asString();
            if (cookies)
                extraHeaders << "Cookie: " << cookies.asString() << "\r\n";

            auto &to = address();
            char errorStr[256];
            mg_error error {errorStr, sizeof(errorStr), 0};
            _connection = mg_connect_websocket_client2(to.hostname.c_str(), to.port,
                                                      to.scheme != "ws" && hasSuffix(to.scheme, "s"),
                                                      &error,
                                                      to.path.c_str(),
                                                      extraHeaders.str().c_str(),
                                                      &connectHandler, &dataHandler, &closeHandler,
                                                      this);
            if (_connection) {
                Debug("CivetWebSocket Connected!");
                retain(this);
                mg_set_user_connection_data(_connection, this);
                delegate().onWebSocketConnect();
            } else {
                // Map civetweb error codes to CloseStatus:
                if (error.code >= MG_ERR_HTTP_STATUS_BASE) {
                    _closeStatus = {kWebSocketClose, error.code - MG_ERR_HTTP_STATUS_BASE};
                } else if (error.code >= MG_ERR_CIVETWEB_BASE) {
                    _closeStatus.reason = kNetworkError;
                    switch (error.code) {
                        case MG_ERR_INVALID_CERT:
                            _closeStatus.code = kNetErrTLSClientCertRejected;
                            break;
                        case MG_ERR_HOST_NOT_FOUND:
                            _closeStatus.code = kNetErrUnknownHost;
                            break;
                        case MG_ERR_DNS_FAILURE:
                            _closeStatus.code = kNetErrDNSFailure;
                            break;
                        default:
                            _closeStatus = {kUnknownError, error.code};
                            break;
                    }
                } else {
                    _closeStatus = {kPOSIXError, error.code};
                }
                _closeStatus.message = errorStr;
                delegate().onWebSocketClose(_closeStatus);
            }
        }


        virtual bool send(fleece::slice message, bool binary) override {
            auto opcode = (binary ? WEBSOCKET_OPCODE_BINARY : WEBSOCKET_OPCODE_TEXT);
            sendFrame(opcode, alloc_slice(message));
            return true;
        }


        virtual void close(int status, fleece::slice message) override {
            enqueue(&CivetWebSocket::_close, status, alloc_slice(message));
        }

        void _close(int status, alloc_slice message) {
            if (_sentCloseFrame)
                return;
            alloc_slice body(2 + message.size);
            *(uint16_t*)body.buf = htons((uint16_t)status);
            memcpy((void*)&body[2], message.buf, message.size);
            _sendFrame(WEBSOCKET_OPCODE_CONNECTION_CLOSE, body);
            _sentCloseFrame = true;
        }

    protected:
        friend class CivetProvider;


        static int connectHandler(const struct mg_connection *connection, void *userData) {
            auto self = (CivetWebSocket*)userData;
            return self->onConnected(connection) ? 0 : 1;
        }

        // civetweb callback: handshake completed (server-side only)
        static void readyHandler(struct mg_connection *connection, void *) {
            auto self = (CivetWebSocket*)mg_get_user_connection_data(connection);
            self->onReady();
        }

        // civetweb callback: received a message
        static int dataHandler(struct mg_connection *connection,
                               int header, char *message, size_t messageLen, void*)
        {
            auto self = (CivetWebSocket*)mg_get_user_connection_data(connection);
            self->onMessage(header & 0x0F, slice(message, messageLen));
            return 1;
        }


        // civetweb callback: TCP socket closed
        static void closeHandler(const struct mg_connection *connection, void*) {
            auto self = (CivetWebSocket*)mg_get_user_connection_data(connection);
            self->enqueue(&CivetWebSocket::_onClosed);
        }

    private:

        void sendFrame(int opcode, alloc_slice body) {
            enqueue(&CivetWebSocket::_sendFrame, opcode, body);
        }

        void _sendFrame(int opcode, alloc_slice body) {
            if (!_connection) return;
            Debug("CivetWebSocket sending a %d frame", opcode);
            auto write = _isServer ? mg_websocket_write : mg_websocket_client_write;
            int ok = write(_connection, opcode, (const char*)body.buf, body.size);
            if (ok <= 0) {
                Warn("mg_websocket_write failed");
                //TODO: What should I do on error?
            }
            if (opcode == WEBSOCKET_OPCODE_TEXT || opcode == WEBSOCKET_OPCODE_BINARY)
                delegate().onWebSocketWriteable();
        }


        bool onConnected(const mg_connection *connection) {
            // Collect the response status & headers:
            auto ri = mg_get_request_info(connection);
            int status = stoi(string(ri->request_uri));
            Debug("CivetWebSocket got HTTP response %d, with %d headers", status, ri->num_headers);

            // Headers can appear more than once, so collect them into an array-valued map:
            unordered_map<string, vector<string>> headerMap;
            for (int i = 0; i < ri->num_headers; i++)
                headerMap[ri->http_headers[i].name].emplace_back(ri->http_headers[i].value);

            // Now encode as a Fleece dict, where values are strings or arrays of strings:
            Encoder enc;
            enc.beginDict(headerMap.size());
            for (auto i = headerMap.begin(); i != headerMap.end(); ++i) {
                enc.writeKey(slice(i->first));
                if (i->second.size() == 1)
                    enc.writeString(i->second[0]);
                else {
                    enc.beginArray();
                    for (string &value : i->second)
                        enc.writeString(value);
                    enc.endArray();
                }
            }
            enc.endDict();
            AllocedDict headers(enc.finish());

            delegate().onWebSocketGotHTTPResponse(status, headers);
            return true;
        }


        void onReady() {
            Debug("CivetWebSocket connected");
            delegate().onWebSocketStart();
        }


        void onMessage(int headerByte, slice message) {
            Debug("CivetWebSocket received a %d frame", (headerByte & 0x0F));
            bool binary = false;
            switch (headerByte & 0x0F) {
                case WEBSOCKET_OPCODE_BINARY:
                    binary = true;
                    // fall through:
                case WEBSOCKET_OPCODE_TEXT:
                    delegate().onWebSocketMessage(message, binary);
                    break;
                case WEBSOCKET_OPCODE_PING:
                    sendFrame(WEBSOCKET_OPCODE_PONG, alloc_slice(message));
                    break;
                case WEBSOCKET_OPCODE_CONNECTION_CLOSE:
                    enqueue(&CivetWebSocket::_onCloseRequest, alloc_slice(message));
                    break;
                default:
                    break;
            }
        }


        void _onClosed() {
            if (!_connection)
                return;
            Debug("CivetWebSocket closed");
            _connection = nullptr;
            if (!_rcvdCloseFrame) {
                _closeStatus.reason = kUnknownError;
            }
            delegate().onWebSocketClose(_closeStatus);
            release(this);
        }


        void _onCloseRequest(alloc_slice body) {
            // https://tools.ietf.org/html/rfc6455#section-7
            _rcvdCloseFrame = true;
            _closeStatus.reason = kWebSocketClose;
            if (body.size >= 2) {
                slice in = body;
                _closeStatus.code = ntohs(*(uint16_t*)in.read(2).buf);
                _closeStatus.message = in;
            } else {
                _closeStatus.code = kCodeStatusCodeExpected;
            }

            if (!_sentCloseFrame) {
                // Peer initiated close, so echo back its reason:
                //TODO: Give the delegate a chance to delay this
                sendFrame(WEBSOCKET_OPCODE_CONNECTION_CLOSE, body);
            }
            mg_close_connection(_connection);
            _onClosed();
        }

        AllocedDict _options;
        struct mg_connection *_connection;
        alloc_slice _responseHeaders;
        CloseStatus _closeStatus;
        bool _isServer;
        bool _accepted {false};
        bool _sentCloseFrame {false};
        bool _rcvdCloseFrame {false};
    };


#pragma mark - PROVIDER:


    CivetProvider& CivetProvider::instance() {
        static CivetProvider p;
        return p;
    }


    void CivetProvider::addProtocol(const string &protocol) {
        _protocols.insert(protocol);
    }


    WebSocket* CivetProvider::createWebSocket(const Address &to,
                                              const AllocedDict &options) {
        return new CivetWebSocket(*this, to, options);
    }


    void CivetProvider::setServerWebSocketHandler(mg_context *context, string uri,
                                                  ServerWebSocketHandler handler)
    {
        auto info = new pair<CivetProvider&,ServerWebSocketHandler>(*this, handler);
        mg_set_websocket_handler(context, uri.c_str(),
                                 &connectHandler,
                                 &CivetWebSocket::readyHandler,
                                 &CivetWebSocket::dataHandler,
                                 &CivetWebSocket::closeHandler,
                                 info);
    }


    int CivetProvider::connectHandler(const mg_connection *roConnection, void *context) {
        auto info = (pair<CivetProvider&,ServerWebSocketHandler>*)context;
        auto socket = new CivetWebSocket(info->first, const_cast<mg_connection*>(roConnection));
        if (!info->second(roConnection, socket)) {
            delete socket;
            return 1; // yes, for some reason 1 means failure...
        }
        return 0;
    }


#pragma mark - C4 SOCKET FACTORY:


    class civetC4Adapter : Delegate {
    public:
        civetC4Adapter(C4Socket *sock, const C4Address *c4To, const AllocedDict &options)
        :c4socket(sock)
        ,socket(CivetProvider::instance().createWebSocket(c4AddressOf(*c4To), options))
        {
            socket->connect(this);
        }

        virtual void onWebSocketGotHTTPResponse(int status, const AllocedDict &headers) override {
            c4socket_gotHTTPResponse(c4socket, status, headers.data());
        }

        virtual void onWebSocketConnect() override {
            c4socket_opened(c4socket);
        }

        virtual void onWebSocketClose(CloseStatus status) override {
            static const C4ErrorDomain kDomainForReason[] = {
                WebSocketDomain, POSIXDomain, NetworkDomain, LiteCoreDomain
            };
            C4ErrorDomain domain = kDomainForReason[status.reason];
            if (status.reason == kUnknownError)
                status.code = kC4ErrorRemoteError;
            socket = nullptr;
            c4socket_closed(c4socket, c4error_make(domain, status.code, status.message));
            c4socket->nativeHandle = nullptr;
            delete this;
        }

        virtual void onWebSocketMessage(fleece::slice message, bool binary) override {
            if (binary)
                c4socket_received(c4socket, message);
        }

        virtual void onWebSocketWriteable() override {
            c4socket_completedWrite(c4socket, _lastWriteSize.exchange(0));
        }

        void send(alloc_slice body, bool binary) {
            _lastWriteSize += body.size;
            socket->send(body, binary);
        }

        C4Socket *c4socket;
        WebSocket* socket;
        atomic<size_t> _lastWriteSize {0};
    };


    static inline civetC4Adapter* internal(C4Socket *sock) {
        return ((civetC4Adapter*)sock->nativeHandle);
    }


    static void sock_open(C4Socket *sock, const C4Address *c4To, FLSlice optionsFleece) {
        sock->nativeHandle = new civetC4Adapter(sock, c4To, AllocedDict((slice)optionsFleece));
    }


    static void sock_write(C4Socket *sock, C4SliceResult allocatedData) {
        internal(sock)->send(alloc_slice(move(allocatedData)), true);
    }

    static void sock_completedReceive(C4Socket *sock, size_t byteCount) {
        //TODO internal(sock)->socket->
    }


    static void sock_requestClose(C4Socket *sock, int status, C4String message) {
        if (internal(sock)->socket)
            internal(sock)->socket->close(status, slice(message));
    }


    C4SocketFactory CivetProvider::C4SocketFactory() {
        return {
            true,
            &sock_open, &sock_write, &sock_completedReceive,
            nullptr,
            &sock_requestClose,
        };
    }


    // Declared in c4Socket+Internal.hh
    const C4SocketFactory C4DefaultSocketFactory {
        true,
        &sock_open,
        &sock_write,
        &sock_completedReceive,
        nullptr,
        &sock_requestClose,
    };

} }
