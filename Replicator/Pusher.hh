//
//  Pusher.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/13/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Replicator.hh"
#include "DBActor.hh"
#include "Actor.hh"
#include "SequenceSet.hh"
#include <queue>

namespace litecore { namespace repl {


    class Pusher : public ReplActor {
    public:
        Pusher(blip::Connection *connection, Replicator *replicator, DBActor *dbActor, Options options);

        void start(C4SequenceNumber sinceSequence);

        // Sent by Replicator in response to dbGetChanges
        void gotChanges(RevList changes, C4Error err) {
            enqueue(&Pusher::_gotChanges, changes, err);
        }

    private:
        bool active() const                         {return _options.push > kC4Passive;}
        virtual bool isBusy() const override;
        virtual void afterEvent() override;
        void startSending(C4SequenceNumber sinceSequence);
        void handleSubChanges(Retained<blip::MessageIn> req);
        void _gotChanges(RevList changes, C4Error err);
        blip::FutureResponse sendChanges(const RevList&);
        void maybeGetMoreChanges();
        void sendChangeList(RevList);
        void sendMoreRevs();
        void sendRevision(const RevRequest&);
        void markComplete(C4SequenceNumber sequence);

        static const unsigned kMaxPossibleAncestorsToSend = 20;
        static const unsigned kMinLengthToCompress = 100;     // Min length body worth compressing
        static const unsigned kDefaultChangeBatchSize = 200;  // # of changes to send in one msg
        static const unsigned kMaxChangeListsInFlight = 4;    // How many changes messages can be active at once
        static const bool kChangeMessagesAreUrgent = true;    // Are change msgs high priority?
        static const unsigned kMaxRevsInFlight = 5;           // # revs to be sending at once

        Replicator* const _replicator;
        DBActor* const _dbActor;
        unsigned _changesBatchSize {kDefaultChangeBatchSize};   // # changes to get from db
        bool _continuous;

        C4SequenceNumber _lastSequence {0};             // Checkpointed last-sequence
        bool _gettingChanges {false};                   // Waiting for _gotChanges() call?
        SequenceSet _pendingSequences;                  // Sequences rcvd from db but not pushed yet
        C4SequenceNumber _lastSequenceRead {0};         // Last sequence read from db
        bool _caughtUp {false};                         // Received backlog of existing changes?
        unsigned _changeListsInFlight {0};              // # 'changes' msgs pending replies
        unsigned _revisionsInFlight {0};                // # 'rev' messages being sent
        std::deque<RevRequest> _revsToSend;             // Revs to send to peer but not sent yet
    };
    
    
} }
