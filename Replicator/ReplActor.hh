//
//  ReplActor.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/20/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Actor.hh"
#include "BLIPConnection.hh"
#include "Message.hh"
#include "Timer.hh"
#include "c4.hh"
#include "c4Replicator.h"
#include <chrono>
#include <functional>


namespace litecore { namespace repl {

    /** Time duration unit: seconds, stored as 64-bit floating point. */
    using duration = std::chrono::nanoseconds;


    /** Abstract base class of Actors used by the replicator */
    class ReplActor : public Actor, InstanceCounted, protected Logging {
    public:
        struct Options {
            using Mode = C4ReplicationMode;

            Mode push {kC4Disabled};
            Mode pull {kC4Disabled};

            duration checkpointSaveDelay        {std::chrono::seconds(5)};

            Options()
            { }
            
            Options(Mode push_, Mode pull_)
            :push(push_), pull(pull_)
            { }

            static Options pushing(Mode mode =kC4OneShot)  {return Options(mode, kC4Disabled);}
            static Options pulling(Mode mode =kC4OneShot)  {return Options(kC4Disabled, mode);}
            static Options passive()                       {return Options(kC4Passive, kC4Passive);}
        };

        /** Called by the Replicator when the BLIP connection closes. */
        void connectionClosed() {
            enqueue(&ReplActor::_connectionClosed);
        }

#if !DEBUG
    protected:
#endif
        blip::Connection* connection() const                {return _connection;}

    protected:
        ReplActor(blip::Connection *connection,
                  Options options,
                  const char *namePrefix);

        /** Registers a callback to run when a BLIP request with the given profile arrives. */
        template <class ACTOR>
        void registerHandler(const char *profile,
                             void (ACTOR::*method)(Retained<blip::MessageIn>)) {
            std::function<void(Retained<blip::MessageIn>)> fn(
                                        std::bind(method, (ACTOR*)this, std::placeholders::_1) );
            _connection->setRequestHandler(profile, asynchronize(fn));
        }

        /** Implementation of connectionClosed(). Maybe overridden, but call super. */
        virtual void _connectionClosed() {
            _connection = nullptr;
        }

        /** Convenience to send a BLIP request. */
        blip::FutureResponse sendRequest(blip::MessageBuilder& builder) {
            return _connection->sendRequest(builder);
        }

        void sendRequest(blip::MessageBuilder& builder,
                         std::function<void(blip::MessageIn*)> callback);

        void gotError(const blip::MessageIn*);
        void gotError(C4Error);

        bool isOpenClient() const           {return _connection && !_connection->isServer();}

        virtual bool isBusy() const;

        virtual std::string loggingIdentifier() const override {
            return actorName();
        }

        Options _options;

    private:
        Retained<blip::Connection> _connection;
        int _pendingResponseCount {0};
    };

} }
