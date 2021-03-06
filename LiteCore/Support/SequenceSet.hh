//
//  SequenceSet.hh
//  LiteCore
//
//  Created by Jens Alfke on 2/27/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include <set>
#include <assert.h>

namespace litecore {

    /** A set of positive integers, generally representing database sequences.
        This is used by the replicator to keep track of which revisions are being pushed. */
    class SequenceSet {
    public:
        typedef uint64_t sequence;
        class reference;

        SequenceSet() { }

        /** Empties the set.
            The optional `max` parameter sets the initial value of the `maxEver` property. */
        void clear(sequence max =0)             {_sequences.clear(); _max = max;}

        bool empty() const                      {return _sequences.empty();}
        size_t size() const                     {return _sequences.size();}

        /** Returns the lowest sequence in the set. If the set is empty, returns 0. */
        sequence first() const                  {return empty() ? 0 : *_sequences.begin();}

        /** The largest sequence ever stored in the set. (The clear() function resets this.) */
        sequence maxEver() const                {return _max;}

        bool contains(sequence s) const         {return _sequences.find(s) != _sequences.end();}

        void add(sequence s)                    {_sequences.insert(s); _max = std::max(_max, s);}
        void remove(sequence s)                 {_sequences.erase(s);}
        void set(sequence s, bool present)      {present ? add(s) : remove(s);}

        reference operator[] (sequence s)               {return reference(*this, s);}
        const reference operator[] (sequence s) const   {return reference(*(SequenceSet*)this, s);}


        // just used as part of the implementation of operator[]
        class reference {
        public:
            operator bool() const                       {return _set.contains(_seq);}
            reference& operator= (bool value)           {_set.set(_seq, value); return *this;}
        protected:
            friend SequenceSet;
            reference(SequenceSet &set, sequence seq)   :_set(set), _seq(seq) { }
            SequenceSet &_set;
            sequence _seq;
        };

    private:
        std::set<sequence> _sequences;
        sequence _max {0};
    };

}
