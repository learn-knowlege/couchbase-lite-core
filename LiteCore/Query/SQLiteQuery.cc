//
//  SQLiteQuery.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "SQLiteKeyStore.hh"
#include "SQLiteDataFile.hh"
#include "SQLite_Internal.hh"
#include "Logging.hh"
#include "Query.hh"
#include "QueryParser.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "Path.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include <sqlite3.h>
#include <sstream>
#include <iostream>

using namespace std;
using namespace fleece;

namespace litecore {


    class SQLiteQuery : public Query {
    public:
        SQLiteQuery(SQLiteKeyStore &keyStore, slice selectorExpression, slice sortExpression)
        :Query(keyStore)
        {
            QueryParser qp(keyStore.tableName());
            qp.parseJSON(selectorExpression, sortExpression);

            stringstream sql;
            sql << "SELECT sequence, key, meta, length(body)";
            _ftsProperties = qp.ftsProperties();
            for (auto property : _ftsProperties) {
                if (!keyStore.hasIndex(property, KeyStore::kFullTextIndex))
                    error::_throw(error::LiteCore, error::NoSuchIndex);
                sql << ", offsets(\"" << keyStore.tableName() << "::" << property << "\")";
            }
            sql << " FROM " << qp.fromClause();
            auto where = qp.whereClause();
            if (!where.empty())
                   sql << " WHERE (" << where << ")";

            auto orderBy = qp.orderByClause();
            if (!orderBy.empty())
                sql << " ORDER BY " << orderBy;

            sql << " LIMIT $limit OFFSET $offset";
            LogTo(SQL, "Compiled Query: %s", sql.str().c_str());
            _statement.reset(keyStore.compile(sql.str()));
        }


        alloc_slice getMatchedText(slice recordID, sequence_t seq) override {
            alloc_slice result;
            if (_ftsProperties.size() > 0) {
                keyStore().get(recordID, kDefaultContent, [&](const Record &rec) {
                    if (rec.body() && rec.sequence() == seq) {
                        auto root = fleece::Value::fromTrustedData(rec.body());
                        //TODO: Support multiple FTS properties in a query
                        auto textObj = fleece::Path::eval(_ftsProperties[0], nullptr, root);
                        if (textObj)
                            result = textObj->asString();
                    }
                });
            }
            return result;
        }
        
        
    protected:
        QueryEnumerator::Impl* createEnumerator(const QueryEnumerator::Options *options) override;
        
        shared_ptr<SQLite::Statement> statement() {return _statement;}

    private:
        friend class SQLiteQueryEnumImpl;

        shared_ptr<SQLite::Statement> _statement;
        vector<string> _ftsProperties;
    };


#pragma mark - QUERY ENUMERATOR:


    class SQLiteQueryEnumImpl : public QueryEnumerator::Impl {
    public:
        SQLiteQueryEnumImpl(SQLiteQuery &query, const QueryEnumerator::Options *options)
        :_query(query)
        ,_statement(query.statement())
        {
            _statement->clearBindings();
            long long offset = 0, limit = -1;
            if (options) {
                offset = options->skip;
                if (options->limit <= INT64_MAX)
                    limit = options->limit;
                if (options->paramBindings.buf)
                    bindParameters(options->paramBindings);
            }
            _statement->bind("$offset", offset);
            _statement->bind("$limit", limit );
            LogStatement(*_statement);
        }

        ~SQLiteQueryEnumImpl() {
            try {
                _statement->reset();
            } catch (...) { }
        }

        void bindParameters(slice json) {
            auto fleeceData = JSONConverter::convertJSON(json);
            const Dict *root = Value::fromData(fleeceData)->asDict();
            if (!root)
                error::_throw(error::InvalidParameter);
            for (Dict::iterator it(root); it; ++it) {
                string key = string("$_") + (string)it.key()->asString();
                const Value *val = it.value();
                try {
                    switch (val->type()) {
                        case kNull:
                            break;
                        case kBoolean:
                        case kNumber:
                            if (val->isInteger() && !val->isUnsigned())
                                _statement->bind(key, (long long)val->asInt());
                            else
                                _statement->bind(key, val->asDouble());
                            break;
                        case kString:
                            _statement->bind(key, (string)val->asString());
                            break;
                        case kData: {
                            slice str = val->asString();
                            _statement->bind(key, str.buf, (int)str.size);
                            break;
                        }
                        default:
                            error::_throw(error::InvalidParameter);
                    }
                } catch (const SQLite::Exception &x) {
                    if (x.getErrorCode() == SQLITE_RANGE)
                        error::_throw(error::InvalidQueryParam);
                }
            }
        }

        bool next(slice &outRecordID, sequence_t &outSequence) override {
            if (!_statement->executeStep())
                return false;
            outSequence = (int64_t)_statement->getColumn(0);
            outRecordID = recordID();
            return true;
        }

        slice recordID() {
            return {(const void*)_statement->getColumn(1), (size_t)_statement->getColumn(1).size()};
        }

        slice meta() override {
            return {_statement->getColumn(2), (size_t)_statement->getColumn(2).size()};
        }
        
        size_t bodyLength() override {
            return (size_t)_statement->getColumn(3).getInt64();
        }

        bool hasFullText() override {
            return _statement->getColumnCount() >= 5;
        }

        void getFullTextTerms(std::vector<QueryEnumerator::FullTextTerm>& terms) override {
            terms.clear();
            // The offsets() function returns a string of space-separated numbers in groups of 4.
            const char *str = _statement->getColumn(4);
            while (*str) {
                uint32_t n[4];
                for (int i = 0; i < 4; ++i) {
                    char *next;
                    n[i] = (uint32_t)strtol(str, &next, 10);
                    str = next;
                }
                terms.push_back({n[1], n[2], n[3]});    // {term #, byte offset, byte length}
            }
         }

        alloc_slice getMatchedText() override {
            return _query.getMatchedText(recordID(), (int64_t)_statement->getColumn(0));
        }

    private:
        SQLiteQuery &_query;
        shared_ptr<SQLite::Statement> _statement;
    };


    QueryEnumerator::Impl* SQLiteQuery::createEnumerator(const QueryEnumerator::Options *options) {
        return new SQLiteQueryEnumImpl(*this, options);
    }


    Query* SQLiteKeyStore::compileQuery(slice selectorExpression, slice sortExpression) {
        ((SQLiteDataFile&)dataFile()).registerFleeceFunctions();
        return new SQLiteQuery(*this, selectorExpression, sortExpression);
    }

}
