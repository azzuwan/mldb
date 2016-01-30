// This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

/** sqlite.cc
    Jeremy Barnes, 9 February 2015
    Copyright (c) 2015 Datacratic Inc.  All rights reserved.

    Implementation of sqlite database.
*/

#include "sqlite_dataset.h"
#include "mldb/arch/rcu_protected.h"
#include "mldb/rest/rest_request_binding.h"
#include "mldb/arch/simd_vector.h"
#include "mldb/jml/utils/worker_task.h"
#include "mldb/sql/sql_expression.h"
#include "mldb/ext/sqlite/sqlite3.h"
#include "mldb/ext/sqlite/sqlite3pp.h"
#include "mldb/ext/sqlite/sqlite3ppext.h"
#include "mldb/jml/utils/lightweight_hash.h"
#include "mldb/types/any_impl.h"


using namespace std;


namespace Datacratic {
namespace MLDB {


void dumpQuery(sqlite3pp::database & db, const std::string & queryStr)
{
    cerr << "dumping query " << queryStr << endl;

    sqlite3pp::query query(db, queryStr.c_str());

    Json::Value allRecords;

    for (sqlite3pp::query::iterator i = query.begin(); i != query.end(); ++i) {
        Json::Value record;
        for (int j = 0; j < query.column_count(); ++j) {
            const char * v = (*i).get<char const*>(j);
            record[query.column_name(j)] = v ? Json::Value(v) : Json::Value();
        }

        allRecords.append(record);
    }

    cerr << allRecords;
}

void explainQuery(sqlite3pp::database & db, const std::string & queryStr)
{
    dumpQuery(db, "EXPLAIN QUERY PLAN " + queryStr);
}

std::string sqlEscape(const std::string & val)
{
    int numQuotes = 0;
    for (char c: val) {
        if (c == '\'')
            ++numQuotes;
        if (c < ' ' || c >= 127)
            throw ML::Exception("Non ASCII character in DB");
    }
    if (numQuotes == 0)
        return val;

    std::string result;
    result.reserve(val.size() + numQuotes);
    
    for (char c: val) {
        if (c == '\'')
            result += "\'\'";
        else result += c;
    }

    return result;
}

namespace {

struct Init {
    Init()
    {
        int res = sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
        if (res != SQLITE_OK)
            throw ML::Exception("Configuring multi threaded: %d", res);
        res = sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0);
        if (res != SQLITE_OK)
            throw ML::Exception("Configuring no memory status: %d", res);
    }
} init;

} // namespace


/*****************************************************************************/
/* SQLITE DATASET CONFIG                                                     */
/*****************************************************************************/

DEFINE_STRUCTURE_DESCRIPTION(SqliteSparseDatasetConfig);

SqliteSparseDatasetConfigDescription::
SqliteSparseDatasetConfigDescription()
{
    addField("dataFileUrl", &SqliteSparseDatasetConfig::dataFileUrl,
             "URI (must be file://) under which the database data lives");
}


/*****************************************************************************/
/* SQLITE INTERNAL REPRESENTATION                                            */
/*****************************************************************************/

struct SqliteSparseDataset::Itl
    : public MatrixView, public ColumnIndex {

    struct Database: public sqlite3pp::database {
        Database(const std::string & filename, const Utf8String & id)
            : sqlite3pp::database(filename.empty() ? ("file::" + id + "?mode=memory&cache=shared").rawData() : filename.c_str())
        {
        }
    };

    struct Connection: public std::unique_ptr<Database> {
        Connection(const Itl * owner, Database * db)
            : std::unique_ptr<Database>(db), owner(owner)
        {
        }

        ~Connection()
        {
            owner->recycleConnection(release());
        }

        Connection(Connection &&) = default;

        const Itl * owner;

    };

    Itl(const Url & url, const Utf8String & id)
    {
        initRoutes();

        if (url.scheme() != "file" && !url.empty())
            throw HttpReturnException(400, "SQLite database requires file:// "
                                      "URI, passed '" + url.toUtf8String() + "'");

        this->filename = url.path();
        this->id = id;

        initDatabase();
    }

    ~Itl()
    {
    }

    std::string filename;
    Utf8String id;

    RestRequestRouter router;

    void initRoutes()
    {
    }

    static void bindArg(sqlite3pp::statement & statement, int index, ColumnHash arg)
    {
        int res = statement.bind(index, sqlite_int64(arg));
        ExcAssertEqual(res, SQLITE_OK);
    }

    static void bindArg(sqlite3pp::statement & statement, int index, RowHash arg)
    {
        int res = statement.bind(index, sqlite_int64(arg));
        ExcAssertEqual(res, SQLITE_OK);
    }

    static void bindArg(sqlite3pp::statement & statement, int index, const RowName & arg)
    {
        int res = statement.bind(index, arg.toString().c_str());
        ExcAssertEqual(res, SQLITE_OK);
    }

    template<typename Arg>
    static void bindArg(sqlite3pp::statement & statement, int index, Arg && arg)
    {
        int res = statement.bind(index, arg);
        ExcAssertEqual(res, SQLITE_OK);
    }

    static void bindArgs(sqlite3pp::statement & statement, int index)
    {
    }

    template<typename First, typename... Args>
    static void bindArgs(sqlite3pp::statement & statement,
                         int index,
                         First&& arg,
                         Args&&... args)
    {
        bindArg(statement, index, arg);
        bindArgs(statement, index + 1, std::forward<Args>(args)...);
    }

    static int decodeQuery(const sqlite3pp::query::rows & rows, int *)
    {
        return rows.get<int>(0);
    }

    static size_t decodeQuery(const sqlite3pp::query::rows & rows, size_t *)
    {
        return rows.get<long long>(0);
    }

    static RowHash decodeQuery(const sqlite3pp::query::rows & rows, RowHash *)
    {
        return RowHash(rows.get<long long>(0));
    }

    static ColumnHash decodeQuery(const sqlite3pp::query::rows & rows, ColumnHash *)
    {
        return ColumnHash(rows.get<long long>(0));
    }

    static Coord decodeQuery(const sqlite3pp::query::rows & rows, Coord *)
    {
        return Coord(rows.get<const char *>(0));
    }

    static std::pair<int, Coord> decodeQuery(const sqlite3pp::query::rows & rows, std::pair<int, Coord> *)
    {
        return make_pair(rows.get<int>(0), Coord(rows.get<const char *>(1)));
    }

    static std::pair<Date, Date>
    decodeQuery(const sqlite3pp::query::rows & rows, std::pair<Date, Date> *)
    {
        return { decodeTs(rows.get<sqlite_int64>(0)),
                decodeTs(rows.get<sqlite_int64>(1)) };
    }

    static std::tuple<ColumnName, CellValue, Date>
    decodeQuery(const sqlite3pp::query::rows & rows, std::tuple<ColumnName, CellValue, Date> *)
    {
        return std::make_tuple(ColumnName(rows.get<const char *>(0)),
                               jsonDecodeStr<CellValue>(Utf8String(rows.get<const char *>(1))),
                               decodeTs(rows.get<long long>(2)));
    }

#if 0
    static std::tuple<RowName, CellValue, Date>
    decodeQuery(const sqlite3pp::query::rows & rows, std::tuple<RowName, CellValue, Date> *)
    {
        return std::make_tuple(RowHash(rows.get<long long>(0)),
                               jsonDecodeStr<CellValue>(Utf8String(rows.get<const char *>(1))),
                               Date::fromSecondsSinceEpoch(rows.get<long long>(2) * 0.001));
    }
#endif    

    template<typename Result, typename... Args>
    std::vector<Result>
    runQuery(const std::string & queryStr, Args&&... args) const
    {
        auto db = getConnection();

        if (false) {
            string explainQuery = "EXPLAIN QUERY PLAN " + queryStr;

            sqlite3pp::query query(*db, explainQuery.c_str());
            bindArgs(query, 1, std::forward<Args>(args)...);
            
            Json::Value explanation;

            for (sqlite3pp::query::iterator i = query.begin(); i != query.end(); ++i) {
                Json::Value record;
                for (int j = 0; j < query.column_count(); ++j) {
                    const char * v = (*i).get<char const*>(j);
                    record[query.column_name(j)] = v ? Json::Value(v) : Json::Value();
                }
                
                explanation.append(record);
            }

            cerr << explainQuery << endl << explanation;
        }

        sqlite3pp::query query(*db, queryStr.c_str());

        bindArgs(query, 1, std::forward<Args>(args)...);

        std::vector<Result> result;

        for (sqlite3pp::query::iterator i = query.begin(); i != query.end(); ++i) {
#if 0
            Json::Value record;
            for (int j = 0; j < query.column_count(); ++j) {
                const char * v = (*i).get<char const*>(j);
                record[query.column_name(j)] = v ? Json::Value(v) : Json::Value();
            }
#endif
            
            result.emplace_back(decodeQuery(*i, (Result *)0));
        }

        return result;
    }

    template<typename Result, typename... Args>
    Result
    runScalarQuery(const std::string & queryStr, Args&&... args) const
    {
        auto res = runQuery<Result>(queryStr, std::forward<Args>(args)...);
        ExcAssertEqual(res.size(), 1);
        return res[0];
    }

    virtual std::vector<RowName>
    getRowNames(ssize_t start = 0, ssize_t limit = -1) const
    {
        string query = "SELECT rowName FROM (SELECT DISTINCT rowName, rowHash FROM rows ORDER BY rowHash";
        if (start != 0)
            query += " OFFSET " + to_string(start);
        if (limit != -1)
            query += " LIMIT " + to_string(limit);
        query += ")";

        return runQuery<RowName>(query);
    }

    virtual std::vector<RowHash>
    getRowHashes(ssize_t start = 0, ssize_t limit = -1) const
    {
        string query = "SELECT rowHash FROM (SELECT DISTINCT rowHash FROM rows ORDER BY rowHash";
        if (start != 0)
            query += " OFFSET " + to_string(start);
        if (limit != -1)
            query += " LIMIT " + to_string(limit);
        query += ")";
        
        return runQuery<RowHash>(query);
    }

    virtual bool knownRow(const RowName & rowName) const
    {
        return runScalarQuery<int>("SELECT EXISTS (SELECT 1 FROM rows WHERE rowHash = ? AND rowName = ?)",
                                   RowHash(rowName), rowName);
    }

    virtual bool knownRowHash(const RowHash & rowHash) const
    {
        return runScalarQuery<int>("SELECT EXISTS (SELECT 1 FROM rows WHERE rowHash = ?)",
                                   rowHash);
    }

    virtual MatrixNamedRow getRow(const RowName & rowName) const
    {
        try {
            auto rowNum = runScalarQuery<int>("SELECT rowNum FROM rows WHERE rowHash = ? AND rowName = ?",
                                              RowHash(rowName), rowName);
            auto cols = runQuery<std::tuple<ColumnName, CellValue, Date> >
                ("SELECT cols.colName, vals.val, vals.ts FROM vals JOIN cols ON vals.colNum = cols.colNum WHERE rowNum = ?", rowNum);

            MatrixNamedRow result;
            result.columns = std::move(cols);
            result.rowName = rowName;
            result.rowHash = rowName;
            return result;
        } catch (...) {

            string queryStr = "SELECT rowNum,rowName,rowHash FROM rows WHERE rowHash = ?";

            sqlite3pp::query query(*getConnection(), queryStr.c_str());
            bindArgs(query, 1, RowHash(rowName));
            
            Json::Value explanation;

            for (sqlite3pp::query::iterator i = query.begin(); i != query.end(); ++i) {
                Json::Value record;
                for (int j = 0; j < query.column_count(); ++j) {
                    const char * v = (*i).get<char const*>(j);
                    record[query.column_name(j)] = v ? Json::Value(v) : Json::Value();
                }
                
                explanation.append(record);
            }

            cerr << queryStr << endl << explanation;
            abort();

            throw;
        }
    }

    virtual RowName getRowName(const RowHash & rowHash) const
    {
        return runScalarQuery<RowName>
            ("SELECT rowName FROM rows WHERE rowHash = ? LIMIT 1", rowHash);
    }

    virtual bool knownColumn(const ColumnName & column) const
    {
        return runScalarQuery<int>("SELECT EXISTS (SELECT 1 FROM cols WHERE colName = ?)",
                                    column);
    }

    virtual ColumnName getColumnName(ColumnHash column) const
    {
        return runScalarQuery<ColumnName>("SELECT colName FROM cols WHERE colHash = ? LIMIT 1",
                                          column);
    }

    /** Return a list of all columns. */
    virtual std::vector<ColumnName> getColumnNames() const
    {
        return runQuery<ColumnName>("SELECT colName FROM (SELECT DISTINCT colHash,colName FROM cols) ORDER BY colHash");
    }

    virtual size_t getRowCount() const
    {
        return runScalarQuery<size_t>("SELECT count(DISTINCT rowName) FROM rows");
    }
    
    virtual size_t getColumnCount() const
    {
        return runScalarQuery<size_t>("SELECT count(DISTINCT colName) FROM cols");
    }
    
    /** Return the value of the column for all rows and timestamps. */
    virtual MatrixColumn getColumn(const ColumnName & column) const
    {
        //auto colNum = runQuery<int>("SELECT colNum FROM cols WHERE colHash = ?",
        //                            column);
        auto rows = runQuery<std::tuple<RowName, CellValue, Date> >
            ("SELECT rows.rowName, vals.val, vals.ts FROM vals JOIN  rows ON vals.rowNum = rows.rowNum AND vals.colNum = (SELECT colNum FROM cols WHERE colName=?)",
             column);
        MatrixColumn result;
        result.rows = std::move(rows);
        result.columnHash = result.columnName = column;
        return result;
    }

    virtual int getRowNum(sqlite3pp::database & db, const RowName & rowName)
    {
        RowHash rowHash(rowName);
        std::string rowNameStr = rowName.toUtf8String().rawString();

        {
            sqlite3pp::command command(db,"INSERT OR IGNORE INTO rows VALUES (NULL, ?, ?)");
            bindArg(command, 1, rowHash);
            bindArg(command, 2, rowNameStr.c_str());
            command.execute();
        }

        //dumpQuery(db, "SELECT * FROM rows");


        if (false) {
            string explainQuery = "EXPLAIN QUERY PLAN SELECT rowNum FROM rows WHERE rowHash = ? LIMIT 1";

            sqlite3pp::query query(db, explainQuery.c_str());
            bindArgs(query, 1, rowHash);
            
            Json::Value explanation;

            for (sqlite3pp::query::iterator i = query.begin(); i != query.end(); ++i) {
                Json::Value record;
                for (int j = 0; j < query.column_count(); ++j) {
                    const char * v = (*i).get<char const*>(j);
                    record[query.column_name(j)] = v ? Json::Value(v) : Json::Value();
                }
                
                explanation.append(record);
            }

            cerr << explainQuery << endl << explanation;
        }



        sqlite3pp::query query(db, "SELECT rowNum FROM rows WHERE rowHash = ? LIMIT 1");
        bindArg(query, 1, rowHash);
        for (sqlite3pp::query::iterator i = query.begin(); i != query.end(); ++i) {
            return (*i).get<int>(0);
        }

        throw HttpReturnException(400, "Couldn't get a row number");
    }

    virtual int getColNum(sqlite3pp::database & db, const ColumnName & colName)
    {
        ColumnHash colHash(colName);
        auto it = colNumCache.find(colHash);
        if (it != colNumCache.end())
            return it->second;

        std::string colNameStr = colName.toUtf8String().rawString();
        
        {
            sqlite3pp::command command(db,"INSERT OR IGNORE INTO cols VALUES (NULL, ?, ?)");
            bindArg(command, 1, colHash);
            bindArg(command, 2, colNameStr.c_str());
            command.execute();
        }

        //dumpQuery(db, "SELECT * FROM cols");

        sqlite3pp::query query(db, "SELECT colNum FROM cols WHERE colHash = ? AND colName = ? LIMIT 1");
        bindArgs(query, 1, colHash, colNameStr.c_str());
        for (sqlite3pp::query::iterator i = query.begin(); i != query.end(); ++i) {
            int result = (*i).get<int>(0);
            colNumCache[colHash] = result;
            return result;
        }

        throw HttpReturnException(400, "Couldn't get a col number");
    }
    
    virtual void
    recordRowItl(const RowName & rowName,
              const std::vector<std::tuple<ColumnName, CellValue, Date> > & vals)
    {
        recordRows({{rowName, vals}});
    }
    
    virtual void recordRows(const std::vector<std::pair<RowName, std::vector<std::tuple<ColumnName, CellValue, Date> > > > & rows)
    {
        std::unique_lock<std::mutex> guard(writeLock);

        auto db = getConnection();

        sqlite3pp::transaction trans(*db);

        sqlite3pp::command command(*db, "INSERT OR IGNORE INTO vals VALUES (?, ?, ?, ?)");

        for (auto & r: rows) {
            const RowName & rowName = r.first;
            const std::vector<std::tuple<ColumnName, CellValue, Date> > & vals = r.second;
            
            Dataset::validateNames(rowName, vals);

            int rowNum = getRowNum(*db, rowName);
            
            for (auto & r: vals) {
                int colNum = getColNum(*db, std::get<0>(r));
                command.reset();
            
                std::string valStr = jsonEncodeUtf8(std::get<1>(r)).rawString();

                command.binder()
                    << rowNum
                    << colNum
                    << sqlite_int64(encodeTs(std::get<2>(r)))
                    << valStr.c_str();

                command.execute();
            }
        }

        trans.commit();
    }

    static int64_t encodeTs(Date ts)
    {
        return ts.secondsSinceEpoch() * 1000;
    }

    static Date decodeTs(int64_t t)
    {
        return Date::fromSecondsSinceEpoch(t * 0.001);
    }

    virtual void commit()
    {
        // Commits happen straight away; no need to do anything special here
    }

    virtual RestRequestMatchResult
    handleRequest(RestConnection & connection,
                  const RestRequest & request,
                  RestRequestParsingContext & context) const
    {
        return router.processRequest(connection, request, context);
    }

    std::pair<Date, Date> getTimestampRange() const
    {
        string query = "SELECT min(ts), max(ts) FROM vals";

        return runScalarQuery<std::pair<Date, Date> >(query);
    }

    Date quantizeTimestamp(Date timestamp) const
    {
        return decodeTs(encodeTs(timestamp));
    }

    void
    initDatabase()
    {
        std::unique_lock<std::mutex> guard(writeLock);

        auto db = getConnection();

        auto doCommand = [&] (const std::string & command)
            {
                int res = db->execute(command.c_str());
                if (res != SQLITE_OK) {
                    throw ML::Exception("Error setting up database: executing %s: %s",
                                        command.c_str(), db->error_msg());
                }
            };

        // Really, really simple table: 6 columns
        doCommand("CREATE TABLE IF NOT EXISTS vals ("
                  "  rowNum INT NOT NULL"
                  ", colNum INT NOT NULL"
                  ", ts BIGINT NOT NULL"
                  ", val TEXT"
                  ", FOREIGN KEY(rowNum) REFERENCES rows(rowNum) ON DELETE CASCADE"
                  ", FOREIGN KEY(colNum) REFERENCES cols(colNum) ON DELETE CASCADE"
                  ")");
        
        doCommand("CREATE TABLE IF NOT EXISTS rows ("
                  "  rowNum INTEGER PRIMARY KEY, "
                  "  rowHash INT NOT NULL, "
                  "  rowName INT NOT NULL "
                  ")");

        doCommand("CREATE TABLE IF NOT EXISTS cols ("
                  "  colNum INTEGER PRIMARY KEY, "
                  "  colHash INT NOT NULL, "
                  "  colName INT NOT NULL "
                  ")");

        doCommand("CREATE UNIQUE INDEX IF NOT EXISTS byrow ON vals (rowNum, colNum, val, ts)");
        doCommand("CREATE INDEX IF NOT EXISTS bycol ON vals (colNum, rowNum, val, ts)");
        doCommand("CREATE INDEX IF NOT EXISTS byts ON vals (ts)");
        doCommand("CREATE UNIQUE INDEX IF NOT EXISTS rownames ON rows (rowHash, rowName)");
        doCommand("CREATE UNIQUE INDEX IF NOT EXISTS colnames ON cols (colHash, colName)");
    }

    // Protected by the write lock
    ML::Lightweight_Hash<RowHash, int> rowNumCache;
    ML::Lightweight_Hash<ColumnHash, int> colNumCache;

    // Unfortunately...
    mutable std::mutex writeLock;

    // Protects our connection pool
    mutable std::mutex connectionsMutex;

    // Connections go here to await for someone to reuse them
    mutable std::vector<std::unique_ptr<Database> > unusedConnections;

    void recycleConnection(Database * connection) const
    {
        if (!connection)
            return;
        std::unique_lock<std::mutex> guard(connectionsMutex);
        unusedConnections.emplace_back(connection);
    }

    void initConnection(Database & connection) const
    {
        auto doCommand = [&] (const std::string & command)
            {
                int res = connection.execute(command.c_str());
                if (res != SQLITE_OK) {
                    throw ML::Exception("Error setting up connection: executing %s: %s",
                                        command.c_str(), connection.error_msg());
                }
            };

        doCommand("PRAGMA busy_timeout=10000");
        doCommand("PRAGMA journal_mode=WAL");
        doCommand("PRAGMA synchronous=NORMAL");
        doCommand("PRAGMA locking_mode=NORMAL");
        doCommand("PRAGMA foreign_keys=ON");
        doCommand("PRAGMA mmap_size=10000000000");
    }

    Connection getConnection() const
    {
        std::unique_lock<std::mutex> guard(connectionsMutex);
        std::unique_ptr<Database> conn;
        if (unusedConnections.empty()) {
            conn.reset(new Database(filename, id));
            initConnection(*conn);
        }
        else {
            conn = std::move(unusedConnections.back());
            unusedConnections.pop_back();
        }
        return Connection(this, conn.release());
    }
};


/*****************************************************************************/
/* SQLITE SPARSE DATASET                                                     */
/*****************************************************************************/

SqliteSparseDataset::
SqliteSparseDataset(MldbServer * owner,
                    PolyConfig config,
                    const std::function<bool (const Json::Value &)> & onProgress)
    : Dataset(owner)
{
    if (!config.params.empty())
        datasetConfig = config.params.convert<SqliteSparseDatasetConfig>();
    itl.reset(new Itl(datasetConfig.dataFileUrl, config.id));
}
    
SqliteSparseDataset::
~SqliteSparseDataset()
{
}

Any
SqliteSparseDataset::
getStatus() const
{
    return Any();
}

void
SqliteSparseDataset::
recordRowItl(const RowName & rowName,
          const std::vector<std::tuple<ColumnName, CellValue, Date> > & vals)
{
    return itl->recordRowItl(rowName, vals);
}

void
SqliteSparseDataset::
recordRows(const std::vector<std::pair<RowName, std::vector<std::tuple<ColumnName, CellValue, Date> > > > & rows)
{
    return itl->recordRows(rows);
}

void
SqliteSparseDataset::
commit()
{
    return itl->commit();
}
    
std::pair<Date, Date>
SqliteSparseDataset::
getTimestampRange() const
{
    return itl->getTimestampRange();
}

Date
SqliteSparseDataset::
quantizeTimestamp(Date timestamp) const
{
    return itl->quantizeTimestamp(timestamp);
}

std::shared_ptr<MatrixView>
SqliteSparseDataset::
getMatrixView() const
{
    return itl;
}

std::shared_ptr<ColumnIndex>
SqliteSparseDataset::
getColumnIndex() const
{
    return itl;
}

RestRequestMatchResult
SqliteSparseDataset::
handleRequest(RestConnection & connection,
              const RestRequest & request,
              RestRequestParsingContext & context) const
{
    return itl->handleRequest(connection, request, context);
}

static RegisterDatasetType<SqliteSparseDataset, SqliteSparseDatasetConfig>
regSqlite(builtinPackage(),
          "sqliteSparse",
          "SQLite-backed fully consistent, persistent, mutable sparse database",
          "datasets/SqliteSparseDataset.md.html");

} // namespace MLDB
} // namespace Datacratic
