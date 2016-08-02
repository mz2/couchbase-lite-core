//
//  Error.hh
//  CBForest
//
//  Created by Jens Alfke on 6/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef CBForest_Error_h
#define CBForest_Error_h

#include <exception>

#undef check

namespace cbforest {

#ifdef _MSC_VER
#define expected(EXPR, VALUE)   (EXPR)
#else
#define expected __builtin_expect
#endif

    /** Most API calls can throw this. */
    struct error : public std::exception {

        enum Domain {
            CBForest,
            POSIX,
            ForestDB,
            SQLite,
            HTTP,
        };

        // Error codes in CBForest domain:
        enum CBForestError {
            AssertionFailed = 1,
            Unimplemented,
            NoSequences,
            UnsupportedEncryption,
            BadRevisionID,
            BadVersionVector,
            CorruptRevisionData,
            CorruptIndexData,
            TokenizerError, // can't create text tokenizer for FTS
        };

        Domain const domain;
        int const code;

        error (Domain d, int c )    :domain(d), code(c) { }
        error (CBForestError e)     :domain(CBForest), code(e) {}

        virtual const char *what() const noexcept override;

        [[noreturn]] static void _throw(Domain d, int c );
        [[noreturn]] static void _throw(CBForestError);
        [[noreturn]] static void _throwHTTPStatus(int status);

        [[noreturn]] static void assertionFailed(const char *func, const char *file, unsigned line,
                                                 const char *expr);
    };


// Like C assert() but throws an exception instead of aborting
#define	CBFAssert(e) \
    (expected(!(e), 0) ? cbforest::error::assertionFailed(__func__, __FILE__, __LINE__, #e) \
                       : (void)0)

// CBFDebugAssert is removed from release builds; use when 'e' test is too expensive
#ifdef NDEBUG
#define CBFDebugAssert(e)   do{ }while(0)
#else
#define CBFDebugAssert(e)   CBFAssert(e)
#endif

}

#endif
