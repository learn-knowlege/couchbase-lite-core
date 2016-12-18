//
//  QueryParser.cc
//  LiteCore
//
//  Created by Jens Alfke on 10/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "QueryParser.hh"
#include "Error.hh"
#include "Fleece.hh"
#include "Path.hh"
#include <utility>
#include <algorithm>

using namespace std;
using namespace fleece;

namespace litecore {


#pragma mark - UTILITY FUNCTIONS:


    [[noreturn]] static void fail(const char *message) {
        throw error(error::LiteCore, error::InvalidQuery, message);
    }

    [[noreturn]] static void fail(const string &message) {
        throw error(error::LiteCore, error::InvalidQuery, message);
    }


    static inline std::ostream& operator<< (std::ostream& o, slice s) {
        o.write((const char*)s.buf, s.size);
        return o;
    }


    static const Array* mustBeArray(const Value *v, const char *elseMessage = "Expected a JSON array") {
        auto a = v ? v->asArray() : nullptr;
        if (!a)
            fail(elseMessage);
        return a;
    }


    // Appends two property-path strings.
    static string appendPaths(const string &parent, string child) {
        if (child[0] == '$') {
            if (child[1] == '.')
                child = child.substr(2);
            else
                child = child.substr(1);
        }
        if (parent.empty())
            return child;
        else if (child[0] == '[')
            return parent + child;
        else
            return parent + "." + child;
    }


    // Writes a string with SQL quoting (inside apostrophes, doubling contained apostrophes.)
    /*static*/ void QueryParser::writeSQLString(std::ostream &out, slice str) {
        out << "'";
        bool simple = true;
        for (unsigned i = 0; i < str.size; i++) {
            if (str[i] == '\'') {
                simple = false;
                break;
            }
        }
        if (simple) {
            out.write((const char*)str.buf, str.size);
        } else {
            for (unsigned i = 0; i < str.size; i++) {
                if (str[i] == '\'')
                    out.write("''", 2);
                else
                    out.write((const char*)&str[i], 1);
            }
        }
        out << "'";
    }

    
    static string propertyFromNode(const Value *node);


#pragma mark - QUERY PARSER TOP LEVEL:


    void QueryParser::reset() {
        _context.clear();
        _context.push_back(&kOuterOperation);
    }


    void QueryParser::parseJSON(slice expressionJSON) {
        alloc_slice expressionFleece = JSONConverter::convertJSON(expressionJSON);
        return parse(Value::fromTrustedData(expressionFleece));
    }
    
    
    void QueryParser::parse(const Value *expression) {
        reset();
        if (expression->asDict()) {
            // Given a dict; assume it's the operands of a SELECT:
            writeSelect(expression->asDict());
        } else {
            const Array *a = expression->asArray();
            if (a && a->count() > 0 && a->get(0)->asString() == "SELECT"_sl) {
                // Given an entire SELECT statement:
                parseNode(expression);
            } else {
                // Given some other expression; treat it as a WHERE clause of an implicit SELECT:
                writeSelect(expression, nullptr);
            }
        }
    }


    void QueryParser::parseJustExpression(const Value *expression) {
        reset();
        parseNode(expression);
    }

    
    void QueryParser::writeSelect(const Dict *operands) {
        writeSelect(operands->get("WHERE"_sl), operands);
    }


    void QueryParser::writeSelect(const Value *where, const Dict *operands) {
        // Have to find all properties involved in MATCH before emitting the FROM clause:
        if (where)
            findFTSProperties(where);

        // 'What' clause:
        _sql << "SELECT";
        int nCol = 0;
        for (auto &col : _baseResultColumns)
            _sql << (nCol++ ? ", " : " ") << col;
        for (auto propertyPath : _ftsProperties) {
            _sql << (nCol++ ? ", " : " ") << "offsets(\"" << _tableName << "::" << propertyPath << "\")";
        }

        auto what = operands ? operands->get("WHAT"_sl) : nullptr;
        if (what)
            fail("WHAT parameter to SELECT isn't supported yet, sorry");
        if (nCol == 0)
            _sql << " *";

        // FROM clause:
        _sql << " FROM ";
        auto from = operands ? operands->get(" FROM"_sl) : nullptr;
        if (from) {
            fail("FROM parameter to SELECT isn't supported yet, sorry");
        } else {
            _sql << _tableName;
            unsigned ftsTableNo = 0;
            for (auto propertyPath : _ftsProperties) {
                _sql << ", \"" << _tableName << "::" << propertyPath << "\" AS FTS" << ++ftsTableNo;
            }
        }

        // WHERE clause:
        if (where) {
            _sql << " WHERE ";
            parseNode(where);
        }

        // ORDER BY clause:
        auto order = operands ? operands->get("ORDER BY"_sl) : nullptr;
        if (order) {
            _sql << " ORDER BY ";
            _context.push_back(&kOrderByOperation); // suppress parens around arg list
            Array::iterator orderBys(mustBeArray(order));
            writeArgList(orderBys);
            _context.pop_back();
        }

        // LIMIT, OFFSET clauses:
        // TODO: Use the ones from operands
        if (!_defaultLimit.empty())
            _sql << " LIMIT " << _defaultLimit;
        if (!_defaultOffset.empty())
            _sql << " OFFSET " << _defaultOffset;
    }


#pragma mark - PARSING:
    
    
    // This table defines the operators and their characteristics.
    // Each operator has a name, min/max argument count, precedence, and a handler method.
    // https://github.com/couchbase/couchbase-lite-core/wiki/JSON-Query-Schema
    // http://www.sqlite.org/lang_expr.html
    typedef void (QueryParser::*OpHandler)(slice op, Array::iterator& args);
    struct QueryParser::Operation {
        slice op; int minArgs; int maxArgs; int precedence; OpHandler handler;};
    const QueryParser::Operation QueryParser::kOperationList[] = {
        {"."_sl,       1, 9,  9,  &QueryParser::propertyOp},
        {"$"_sl,       1, 1,  9,  &QueryParser::parameterOp},

        {"||"_sl,      2, 9,  8,  &QueryParser::infixOp},

        {"*"_sl,       2, 9,  7,  &QueryParser::infixOp},
        {"/"_sl,       2, 2,  7,  &QueryParser::infixOp},
        {"%"_sl,       2, 2,  7,  &QueryParser::infixOp},

        {"+"_sl,       2, 9,  6,  &QueryParser::infixOp},
        {"-"_sl,       2, 2,  6,  &QueryParser::infixOp},
        {"-"_sl,       1, 1,  9,  &QueryParser::prefixOp},

        {"<"_sl,       2, 2,  4,  &QueryParser::infixOp},
        {"<="_sl,      2, 2,  4,  &QueryParser::infixOp},
        {">"_sl,       2, 2,  4,  &QueryParser::infixOp},
        {">="_sl,      2, 2,  4,  &QueryParser::infixOp},

        {"="_sl,       2, 2,  3,  &QueryParser::infixOp},
        {"!="_sl,      2, 2,  3,  &QueryParser::infixOp},
        {"IS"_sl,      2, 2,  3,  &QueryParser::infixOp},
        {"IS NOT"_sl,  2, 2,  3,  &QueryParser::infixOp},
        {"IN"_sl,      2, 9,  3,  &QueryParser::inOp},
        {"NOT IN"_sl,  2, 9,  3,  &QueryParser::inOp},
        {"LIKE"_sl,    2, 2,  3,  &QueryParser::infixOp},
        {"MATCH"_sl,   2, 2,  3,  &QueryParser::matchOp},
        {"BETWEEN"_sl, 3, 3,  3,  &QueryParser::betweenOp},
        {"EXISTS"_sl,  1, 1,  8,  &QueryParser::existsOp},

        {"NOT"_sl,     1, 1,  9,  &QueryParser::prefixOp},
        {"AND"_sl,     2, 9,  2,  &QueryParser::infixOp},
        {"OR"_sl,      2, 9,  2,  &QueryParser::infixOp},

        {"SELECT"_sl,  1, 1,  1,  &QueryParser::selectOp},

        {"DESC"_sl,    1, 1,  2,  &QueryParser::postfixOp},

        {nullslice,    0, 0, 10,  &QueryParser::fallbackOp} // fallback; must come last
    };

    const QueryParser::Operation QueryParser::kArgListOperation
        {","_sl,       0, 9, -2, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kOrderByOperation
        {"ORDER BY"_sl,1, 9, -3, &QueryParser::infixOp};
    const QueryParser::Operation QueryParser::kOuterOperation
        {nullslice,    1, 1, -1};


    void QueryParser::parseNode(const Value *node) {
        switch (node->type()) {
            case kNull:
                _sql << "null";     //TODO: How do we represent JSON null in SQL?
            case kNumber:
                _sql << node->toString();
                break;
            case kBoolean:
                _sql << (node->asBool() ? '1' : '0');    // SQL doesn't have true/false
                break;
            case kString:
                writeSQLString(node->asString());
                break;
            case kData:
                fail("Binary data not supported in query");
            case kArray:
                parseOpNode((const Array*)node);
                break;
            case kDict:
                fail("Dictionaries not supported in query");
                break;
        }
    }


    void QueryParser::parseOpNode(const Array *node) {
        Array::iterator array(node);
        if (array.count() == 0)
            fail("Empty JSON array");
        slice op = array[0]->asString();
        if (!op)
            fail("Operation must be a string");
        ++array;

        // Look up the handler:
        int nargs = min(array.count(), 9u);
        bool nameMatched = false;
        const Operation *def;
        for (def = kOperationList; def->op; ++def) {
            if (op == def->op) {
                nameMatched = true;
                if (nargs >= def->minArgs && nargs <= def->maxArgs)
                    break;
            }
        }
        if (nameMatched && !def->op)
            fail(string("Wrong number of arguments to ") + (string)op);
        handleOperation(def, op, array);
    }


    // Invokes an Operation's handler. Pushes Operation on the stack and writes parens if needed
    void QueryParser::handleOperation(const Operation* op,
                                      slice actualOperator,
                                      Array::iterator& operands)
    {
        bool parenthesize = (op->precedence <= _context.back()->precedence);
        _context.push_back(op);
        if (parenthesize)
            _sql << '(';

        auto handler = op->handler;
        (this->*handler)(actualOperator, operands);

        if (parenthesize)
            _sql << ')';
        _context.pop_back();
    }


#pragma mark - OPERATION HANDLERS:


    // Handles prefix (unary) operators
    void QueryParser::prefixOp(slice op, Array::iterator& operands) {
        _sql << op;
        if (isalpha(op[op.size-1]))
            _sql << ' ';
        parseNode(operands[0]);
    }


    // Handles postfix operators
    void QueryParser::postfixOp(slice op, Array::iterator& operands) {
        parseNode(operands[0]);
        _sql << " " << op;
    }

    
    // Handles infix operators
    void QueryParser::infixOp(slice op, Array::iterator& operands) {
        int n = 0;
        for (auto &i = operands; i; ++i) {
            if (n++ > 0) {
                if (op != ","_sl)           // special case for argument lists
                    _sql << ' ';
                _sql << op << ' ';
            }
            parseNode(i.value());
        }
    }


    // Handles EXISTS
    void QueryParser::existsOp(slice op, Array::iterator& operands) {
        // "EXISTS propertyname" turns into a call to fl_exists()
        if (writeNestedPropertyOpIfAny("fl_exists", operands))
            return;

        _sql << op;
        if (isalpha(op[op.size-1]))
            _sql << ' ';
        parseNode(operands[0]);
    }

    
    // Handles "x BETWEEN y AND z" expressions
    void QueryParser::betweenOp(slice op, Array::iterator& operands) {
        parseNode(operands[0]);
        _sql << ' ' << op << ' ';
        parseNode(operands[1]);
        _sql << " AND ";
        parseNode(operands[2]);
    }


    // Handles "x IN y" and "x NOT IN y" expressions
    void QueryParser::inOp(slice op, Array::iterator& operands) {
        parseNode(operands.value());
        _sql << ' ' << op << ' ';
        writeArgList(++operands);
    }


    // Handles "property MATCH pattern" expressions (FTS)
    void QueryParser::matchOp(slice op, Array::iterator& operands) {
        string property = propertyFromNode(operands[0]);
        if (property.empty())
            fail("Source of MATCH must be a property");
        // Write the match expression (using an implicit join):
        auto ftsTableNo = AddFTSPropertyIndex((string)property);
        _sql << "(FTS" << ftsTableNo << ".text MATCH ";
        parseNode(operands[1]);
        _sql << " AND FTS" << ftsTableNo << ".rowid = " << _tableName << ".sequence)";
    }

    
    // Handles document property accessors, e.g. [".", "prop"] --> fl_value(body, "prop")
    void QueryParser::propertyOp(slice op, Array::iterator& operands) {
        writePropertyOp("fl_value", operands);
    }


    // Handles substituted query parameters, e.g. ["$", "x"] --> $_x
    void QueryParser::parameterOp(slice op, Array::iterator& operands) {
        auto operand = operands[0];
        switch (operand->type()) {
            case kNumber:
            case kString: {
                // TODO: Validate name
                string parameter = (string)operand->toString();
                _parameters.insert(parameter);
                _sql << "$_" << parameter;
                break;
            }
            default:
                fail("Query parameter name must be number or string");
        }
    }


    // Handles SELECT
    void QueryParser::selectOp(fleece::slice op, Array::iterator &operands) {
        // SELECT is unusual in that its operands are encoded as an object
        auto dict = operands[0]->asDict();
        if (!dict)
            fail("Argument to SELECT must be an object");
        if (_context.size() <= 2) {
            // Outer SELECT
            writeSelect(dict);
        } else {
            // Nested SELECT; use a fresh parser
            QueryParser nested(_tableName, _bodyColumnName);
            nested.parse(dict);
            _sql << nested.SQL();
        }
    }


    // Handles unrecognized operators. If op ends in "()" it's a function call; else fail.
    void QueryParser::fallbackOp(slice op, Array::iterator& operands) {
        if (!(op.size > 2 && op[op.size-2] == '(' && op[op.size-1] == ')'))
            fail(string("Unknown operator: ") + (string)op);

        auto operation = *_context.back();
        operation.op = op;
        _context.back() = &operation;

        op.size -= 2;
        // TODO: Validate that this is a known function

        // Special case: "count(propertyname)" turns into a call to fl_count:
        if (op == "count"_sl && writeNestedPropertyOpIfAny("fl_count", operands))
            return;
        else if (op == "rank"_sl && writeNestedPropertyOpIfAny("rank", operands)) {
            return;
        }

        _sql << op;
        writeArgList(operands);
    }


    // Writes operands as a comma-separated list (parenthesized depending on current precedence)
    void QueryParser::writeArgList(Array::iterator& operands) {
        handleOperation(&kArgListOperation, kArgListOperation.op, operands);
    }


#pragma mark - PROPERTIES:


    // Given the operands of a valid property node, returns the property
    static string propertyFromNode(Array::iterator &operands) {
        stringstream property;
        int n = 0;
        for (auto &i = operands; i; ++i,++n) {
            auto item = i.value();
            auto arr = item->asArray();
            if (arr) {
                if (n == 0)
                    fail("Property path can't start with an array index");
                // TODO: Support ranges (2 numbers)
                if (arr->count() != 1)
                    fail("Property array index must have exactly one item");
                auto index = arr->get(0)->asInt();
                property << '[' << index << ']';
            } else {
                slice name = item->asString();
                if (!name)
                    fail("Invalid JSON value in property path");
                // TODO: Validate name
                if (n > 0)
                    property << '.';
                property << name;
            }
        }
        return property.str();
    }


    // Returns the property represented by a node, or "" if it's not a property node
    static string propertyFromNode(const Value *node) {
        Array::iterator i(node->asArray());
        if (i.count() < 2 || i[0]->asString() != "."_sl)
            return "";              // not a valid property node
        ++i;  // skip "." item
        return propertyFromNode(i);
    }


    // Writes a property-access function call given the JSON operands and a SQL fn name
    void QueryParser::writePropertyOp(const char *fnName, Array::iterator& operands) {
        writePropertyGetter(fnName, propertyFromNode(operands));
    }


    // If the first operand is a property operation, writes it using the given SQL function name
    // and returns true; else returns false.
    bool QueryParser::writeNestedPropertyOpIfAny(const char *fnName, Array::iterator &operands) {
        if (operands.count() == 0 )
            return false;
        auto arr = operands[0]->asArray();
        if (!arr || arr->count() == 0)
            return false;
        Array::iterator nestedOperands(arr);
        if (nestedOperands[0]->asString() != "."_sl)
            return false;

        ++nestedOperands; // skip "."
        writePropertyOp(fnName, nestedOperands);
        return true;
    }


    // Writes a call to a Fleece SQL function, including the closing ")".
    void QueryParser::writePropertyGetter(const string &fn, const string &property) {
        if (property == "_id") {
            if (fn != "fl_value")
                fail("can't use '_id' in this context");
            _sql << "key";
        } else if (property == "_sequence") {
            if (fn != "fl_value")
                fail("can't use '_sequence' in this context");
            _sql << "sequence";
        } else if (fn == "rank") {
            // FTS rank() needs special treatment
            if (FTSPropertyIndex(property) == 0)
                fail("rank() can only be used with FTS properties");
            _sql << "rank(matchinfo(\"" << _tableName << "::" << property << "\"))";
        } else {
            _sql << fn << "(" << _bodyColumnName << ", ";
            auto path = appendPaths(_propertyPath, property);
            writeSQLString(_sql, slice(path));
            _sql << ")";
        }
    }


    /*static*/ string QueryParser::propertyGetter(slice property, const char *bodyColumnName) {
        QueryParser qp("XXX", bodyColumnName);
        qp.writePropertyGetter("fl_value", (string)property);
        return qp.SQL();
    }


#pragma mark - FULL-TEXT-SEARCH MATCH:


    void QueryParser::findFTSProperties(const Value *node) {
        Array::iterator i(node->asArray());
        if (i.count() == 0)
            return;
        slice op = i.value()->asString();
        ++i;
        if (op == "MATCH"_sl && i) {
            auto propertyNode = i.value()->asArray();
            if (propertyNode) {
                string property = propertyFromNode(propertyNode);
                AddFTSPropertyIndex(property);
            }
            ++i;
        }

        // Recurse into operands:
        for (; i; ++i)
            findFTSProperties(i.value());
    }


    size_t QueryParser::FTSPropertyIndex(const string &propertyPath) {
        auto i = find(_ftsProperties.begin(), _ftsProperties.end(), propertyPath);
        if (i == _ftsProperties.end())
            return 0;
        return i - _ftsProperties.begin() + 1;
    }


    size_t QueryParser::AddFTSPropertyIndex(const string &property) {
        // The FTS index is a separate (virtual) table. Get the table name and add it to FROM:
        auto propertyPath = appendPaths(_propertyPath, property);
        size_t index = FTSPropertyIndex(propertyPath);
        if (index == 0) {
            _ftsProperties.push_back(propertyPath);
            index = _ftsProperties.size();
        }
        return index;
    }

}