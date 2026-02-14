// SPDX-License-Identifier: MIT

#pragma once

#include <libpq-fe.h>
#include <memory>

namespace dbwriter {

// Wrapper interface for libpq functions to enable testing
class ILibPq {
public:
    virtual ~ILibPq() = default;

    // Connection
    virtual PGconn* connectdb(const char* conninfo) = 0;
    virtual void finish(PGconn* conn) = 0;
    virtual ConnStatusType status(const PGconn* conn) = 0;
    virtual int socket(const PGconn* conn) = 0;
    virtual int setnonblocking(PGconn* conn, int arg) = 0;
    virtual char* errorMessage(const PGconn* conn) = 0;

    // Async query
    virtual int sendQuery(PGconn* conn, const char* query) = 0;
    virtual int flush(PGconn* conn) = 0;
    virtual int consumeInput(PGconn* conn) = 0;
    virtual int isBusy(PGconn* conn) = 0;
    virtual PGresult* getResult(PGconn* conn) = 0;

    // Result handling
    virtual ExecStatusType resultStatus(const PGresult* res) = 0;
    virtual char* resultErrorMessage(const PGresult* res) = 0;
    virtual void clear(PGresult* res) = 0;
    virtual int ntuples(const PGresult* res) = 0;
    virtual int nfields(const PGresult* res) = 0;
    virtual char* getvalue(const PGresult* res, int row, int col) = 0;
    virtual int getisnull(const PGresult* res, int row, int col) = 0;
    virtual char* fname(const PGresult* res, int col) = 0;

    // COPY
    virtual int putCopyData(PGconn* conn, const char* buffer, int nbytes) = 0;
    virtual int putCopyEnd(PGconn* conn, const char* errormsg) = 0;

    // Command result
    virtual char* cmdTuples(PGresult* res) = 0;
};

// Real implementation that delegates to actual libpq
class RealLibPq : public ILibPq {
public:
    PGconn* connectdb(const char* conninfo) override {
        return PQconnectdb(conninfo);
    }
    void finish(PGconn* conn) override {
        PQfinish(conn);
    }
    ConnStatusType status(const PGconn* conn) override {
        return PQstatus(conn);
    }
    int socket(const PGconn* conn) override {
        return PQsocket(conn);
    }
    int setnonblocking(PGconn* conn, int arg) override {
        return PQsetnonblocking(conn, arg);
    }
    char* errorMessage(const PGconn* conn) override {
        return PQerrorMessage(conn);
    }
    int sendQuery(PGconn* conn, const char* query) override {
        return PQsendQuery(conn, query);
    }
    int flush(PGconn* conn) override {
        return PQflush(conn);
    }
    int consumeInput(PGconn* conn) override {
        return PQconsumeInput(conn);
    }
    int isBusy(PGconn* conn) override {
        return PQisBusy(conn);
    }
    PGresult* getResult(PGconn* conn) override {
        return PQgetResult(conn);
    }
    ExecStatusType resultStatus(const PGresult* res) override {
        return PQresultStatus(res);
    }
    char* resultErrorMessage(const PGresult* res) override {
        return PQresultErrorMessage(res);
    }
    void clear(PGresult* res) override {
        PQclear(res);
    }
    int ntuples(const PGresult* res) override {
        return PQntuples(res);
    }
    int nfields(const PGresult* res) override {
        return PQnfields(res);
    }
    char* getvalue(const PGresult* res, int row, int col) override {
        return PQgetvalue(res, row, col);
    }
    int getisnull(const PGresult* res, int row, int col) override {
        return PQgetisnull(res, row, col);
    }
    char* fname(const PGresult* res, int col) override {
        return PQfname(res, col);
    }
    int putCopyData(PGconn* conn, const char* buffer, int nbytes) override {
        return PQputCopyData(conn, buffer, nbytes);
    }
    int putCopyEnd(PGconn* conn, const char* errormsg) override {
        return PQputCopyEnd(conn, errormsg);
    }
    char* cmdTuples(PGresult* res) override {
        return PQcmdTuples(res);
    }
};

// Global instance (can be swapped for testing)
inline ILibPq& GetLibPq() {
    static RealLibPq instance;
    return instance;
}

}  // namespace dbwriter
