//
//  Response.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Response.hh"
#include "Writer.hh"
#include "StringUtil.hh"
#include "civetUtils.hh"
#include "civetweb.h"

using namespace std;
using namespace fleece;
using namespace fleeceapi;

namespace litecore { namespace REST {

    Body::Body(mg_connection *conn)
    :_conn(conn)
    { }


    slice Body::header(const char *header) const {
        return slice(mg_get_header(_conn, header));
    }
    
    
    std::string Body::urlDecode(const std::string &str) {
        std::string result;
        result.reserve(str.size());
        litecore::REST::urlDecode(str.data(), str.size(), result, false);
        return result;
    }


    std::string Body::urlEncode(const std::string &str) {
        std::string result;
        result.reserve(str.size() + 16);
        litecore::REST::urlEncode(str.data(), str.size(), result, false);
        return result;
    }


    bool Body::hasContentType(slice contentType) const {
        slice actualType = header("Content-Type");
        return actualType.size >= contentType.size
        && memcmp(actualType.buf, contentType.buf, contentType.size) == 0
        && (actualType.size == contentType.size || actualType[contentType.size] == ';');
    }


    alloc_slice Body::body() const {
        if (!_gotBody) {
            fleece::Writer writer;
            int bytesRead;
            do {
                char buf[1024];
                bytesRead = mg_read(_conn, buf, sizeof(buf));
                if (bytesRead > 0)
                    writer.write(buf, bytesRead);
            } while (bytesRead > 0);
            if (bytesRead < 0)
                return {};
            alloc_slice body = writer.extractOutput();
            if (body.size == 0)
                body.reset();
            const_cast<Body*>(this)->_body = body;
            const_cast<Body*>(this)->_gotBody = true;
        }
        return _body;
    }


    Value Body::bodyAsJSON() const {
        if (!_gotBodyFleece) {
            if (hasContentType("application/json"_sl)) {
                alloc_slice b = body();
                if (b)
                    const_cast<Body*>(this)->_bodyFleece =
                    JSONEncoder::convertJSON(b, nullptr);
            }
            const_cast<Body*>(this)->_gotBodyFleece = true;
        }
        return _bodyFleece ? Value::fromData(_bodyFleece) : nullptr;
    }


#pragma mark - RESPONSE:


    static mg_connection* sendRequest(const std::string &method,
                                      const std::string &hostname,
                                      uint16_t port,
                                      const std::string &uri,
                                      const std::map<std::string, std::string> &headers,
                                      fleece::slice body,
                                      string &errorMessage,
                                      int &errorCode)
    {
        stringstream hdrs;
        if (!headers.empty()) {
            for (auto &header : headers)
                hdrs << header.first << ": " << header.second << "\r\n";
            hdrs << "Content-Length: " << body.size << "\r\n";
        }
        char errorBuf[256];
        mg_error error {errorBuf, sizeof(errorBuf), 0};
        auto conn = mg_download(hostname.c_str(), port, false, &error,
                                "%s %s HTTP/1.0\r\n%s\r\n%.*s",
                                method.c_str(), uri.c_str(), hdrs.str().c_str(), SPLAT(body));
        if (!conn) {
            errorMessage = string(errorBuf);
            errorCode = error.code;
        }
        return conn;
    }


    Response::Response(const std::string &method,
                       const std::string &hostname,
                       uint16_t port,
                       const std::string &uri,
                       const std::map<std::string, std::string> &headers,
                       fleece::slice body)
    :Body(sendRequest(method, hostname, port, uri, headers, body, _errorMessage, _errorCode))
    { }


    Response::Response(const string &method,
                       const string &hostname,
                       uint16_t port,
                       const string &uri,
                       slice body)
    :Response(method, hostname, port, uri, {}, body)
    { }


    Response::~Response() {
        if (_conn)
            mg_close_connection(_conn);
    }

    HTTPStatus Response::status() const {
        if (_conn)
            return (HTTPStatus) stoi(string(mg_get_request_info(_conn)->request_uri));
        else
            return HTTPStatus::undefined;
    }

    std::string Response::statusMessage() const {
        if (_conn)
            return string(mg_get_request_info(_conn)->http_version);
        else
            return _errorMessage;
    }


} }
