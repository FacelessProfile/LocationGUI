#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <atomic>
#include <algorithm>
#include <libpq-fe.h>
#include <zmq.hpp>
#include "headers/data.h"
#include "headers/AltitudeService.h"

#define HOST "localhost"
#define PORT "5432"
#define DB_NAME "location"
#define DB_USER "postgres"
#define DB_USER_PASSWORD "strongpassword"
#define ZMQ_PORT "20077"

const char* sanitize(const std::string& val) {
    if (val == "2147483647" || val == "2147483648" || val.empty()) return nullptr;
    return val.c_str();
}

void AppendData(PGconn *con, const char **values, int size) {
    std::string query = "INSERT INTO telemetry "
        "(lat, lon, timestamp, net_type, band, cell_id, pci, tac, mcc, mnc, rsrp, rsrq, rssi, snr, cqi) "
        "VALUES ($1, $2, to_timestamp($3), $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15)";
    
    PGresult* res = PQexecParams(con, query.c_str(), size, NULL, values, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::cerr << "\033[31mDB ERROR:\033[0m " << PQresultErrorMessage(res) << "\n";
    }
    PQclear(res);
}

std::vector<TelemetryData> FetchInitialData() {
    std::vector<TelemetryData> init_list;
    const char* info = "host=" HOST " port=" PORT " dbname=" DB_NAME " user=" DB_USER " password=" DB_USER_PASSWORD;
    PGconn *con = PQconnectdb(info);

    if (PQstatus(con) != CONNECTION_OK) {
        PQfinish(con);
        return init_list; 
    }

    const char* query = "SELECT lat, lon, EXTRACT(EPOCH FROM timestamp)::text, net_type, rsrp, cell_id "
                        "FROM telemetry ORDER BY timestamp DESC LIMIT 500";
    
    PGresult *res = PQexec(con, query);
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        for (int i = PQntuples(res) - 1; i >= 0; i--) { 
            TelemetryData d;
            d.lat = PQgetvalue(res, i, 0);
            d.lon = PQgetvalue(res, i, 1);
            d.timestamp = PQgetvalue(res, i, 2);
            d.net_type = PQgetvalue(res, i, 3);
            d.signal = PQgetvalue(res, i, 4);
            d.cell_info = std::string("CID:") + PQgetvalue(res, i, 5);
            init_list.push_back(d);
        }
    }
    PQclear(res);
    PQfinish(con);
    return init_list;
}

std::vector<TelemetryData> FetchMapData() {
    std::vector<TelemetryData> map_list;
    PGconn *con = PQconnectdb("host=" HOST " port=" PORT " dbname=" DB_NAME " user=" DB_USER " password=" DB_USER_PASSWORD);
    
    if (PQstatus(con) != CONNECTION_OK) {
        PQfinish(con);
        return map_list;
    }
    const char* query = "SELECT lat, lon, rsrp, net_type, EXTRACT(EPOCH FROM timestamp)::text, "
                        "band, cell_id, pci, tac, mcc, mnc, rsrq, rssi, snr, cqi, band AS earfcn, "
                        "COALESCE(alt::text, '') AS alt FROM telemetry";

    PGresult* res = PQexec(con, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "FetchMapData: " << PQresultErrorMessage(res) << "\n";
        PQclear(res);
        PQfinish(con);
        return map_list;
    }

    const int rows = PQntuples(res);
    map_list.reserve(rows);
    for (int i = 0; i < rows; i++) {
        TelemetryData td;
        td.lat = PQgetvalue(res, i, 0);
        td.lon = PQgetvalue(res, i, 1);
        td.signal = PQgetvalue(res, i, 2);
        td.net_type = PQgetvalue(res, i, 3);
        td.timestamp = PQgetvalue(res, i, 4);
        td.band = PQgetvalue(res, i, 5);
        td.cell_id = PQgetvalue(res, i, 6);
        td.pci = PQgetvalue(res, i, 7);
        td.tac = PQgetvalue(res, i, 8);
        td.mcc = PQgetvalue(res, i, 9);
        td.mnc = PQgetvalue(res, i, 10);
        td.rsrq = PQgetvalue(res, i, 11);
        td.rssi = PQgetvalue(res, i, 12);
        td.snr = PQgetvalue(res, i, 13);
        td.cqi = PQgetvalue(res, i, 14);
        td.earfcn = PQgetvalue(res, i, 15);
        td.alt = PQgetvalue(res, i, 16);
        map_list.push_back(td);
    }
    PQclear(res);

    AltitudeService::BackfillMissingAltitudes(con, map_list);

    PQfinish(con);
    return map_list;
}

void RunNetworkModule(SharedBuffer& shared_buffer, std::atomic<bool>& should_run) {
    PGconn *con = PQconnectdb("host=" HOST " port=" PORT " dbname=" DB_NAME " user=" DB_USER " password=" DB_USER_PASSWORD);

    if (PQstatus(con) != CONNECTION_OK) {
        should_run = false;
        return;
    }

    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.bind("tcp://*:" ZMQ_PORT);
    socket.set(zmq::sockopt::rcvtimeo, 500);

    while (should_run) {
        zmq::message_t request;
        if (!socket.recv(request, zmq::recv_flags::none)) continue;

        std::string msg = request.to_string();
        std::stringstream ss(msg);
        std::vector<std::string> tokens;
        std::string token;
        
        while (std::getline(ss, token, ';')) tokens.push_back(token);

        if (tokens.size() >= 16) {
        	if (tokens[0] == "SKIP" || tokens[1] == "SKIP") {
                std::string flags = shared_buffer.getFlags(); 
                socket.send(zmq::message_t(flags.begin(), flags.end()), zmq::send_flags::none);
                continue;
            }
            std::string net_type = tokens[5];
            std::string band, cell_id, pci, tac, mcc, mnc, rsrp, rsrq, rssi, snr, cqi;

            // Корректный маппинг в зависимости от типа сети
            if (net_type == "LTE" && tokens.size() >= 19) {
                band = tokens[6]; cell_id = tokens[7]; 
                mcc = tokens[9]; mnc = tokens[10]; pci = tokens[11]; tac = tokens[12];
                cqi = tokens[14]; rsrp = tokens[15]; rsrq = tokens[16]; rssi = tokens[17]; snr = tokens[18];
            } else if (net_type == "GSM" && tokens.size() >= 15) {
                cell_id = tokens[6]; pci = tokens[7]; // У GSM это bsic, он как раз 0-63
                band = tokens[8]; // arfcn
                tac = tokens[9]; // lac
                mcc = tokens[10]; mnc = tokens[11];
                rsrp = tokens[13]; rssi = tokens[14]; // rssi и сигнал
            } else if (net_type == "NR" && tokens.size() >= 16) {
                band = tokens[6]; cell_id = tokens[7]; pci = tokens[8];
                tac = tokens[10]; mcc = tokens[11]; mnc = tokens[12];
                rsrp = tokens[13]; rsrq = tokens[14]; snr = tokens[15];
            } else {
                // Фолбэк на случай неизвестного формата
                if (tokens.size() > 6) band = tokens[6];
                if (tokens.size() > 7) cell_id = tokens[7];
                if (tokens.size() > 11) pci = tokens[11];
                if (tokens.size() > 15) rsrp = tokens[15];
            }

            const char* db_params[] = {
                tokens[0].c_str(),  // lat
                tokens[1].c_str(),  // lon
                sanitize(tokens[2]), //alt
                tokens[3].c_str(),  // timestamp
                net_type.c_str(),   // net_type
                sanitize(band),     // band
                sanitize(cell_id),  // cell_id
                sanitize(pci),      // pci 
                sanitize(tac),      // tac
                sanitize(mcc),      // mcc
                sanitize(mnc),      // mnc
                sanitize(rsrp),     // rsrp
                sanitize(rsrq),     // rsrq
                sanitize(rssi),     // rssi
                sanitize(snr),      // snr
                sanitize(cqi)       // cqi
            };

            AppendData(con, db_params, 15);

            TelemetryData td;
            td.lat = tokens[0];
            td.lon = tokens[1];
            td.alt = sanitize(tokens[2]);
            td.net_type = net_type;
            td.signal = rsrp.empty() ? "0" : rsrp;
            td.timestamp = tokens[3];
            td.cell_info = "CID:" + cell_id + " PCI:" + pci;
            
            shared_buffer.addData(td);
            
            std::string flags = shared_buffer.getFlags(); 
            socket.send(zmq::message_t(flags.begin(), flags.end()), zmq::send_flags::none);
        } else {
            socket.send(zmq::str_buffer("SKIP"), zmq::send_flags::none);
        }
    }
    PQfinish(con);
}
