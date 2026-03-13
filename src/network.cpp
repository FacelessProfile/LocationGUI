#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <atomic>
#include <libpq-fe.h>
#include <zmq.hpp>
#include "headers/data.h"

#define HOST "localhost"
#define PORT "5432"
#define DB_NAME "location"
#define DB_USER "postgres"
#define DB_USER_PASSWORD "BibaBoba34505"
#define ZMQ_PORT "20077"

void AppendData(PGconn *con, const std::string& table, const char **Recvd_values, const int Recvd_size) {
    std::string query = "INSERT INTO " + table +
                        "(lat, lon, altitude, capture_time, accuracy, net_type, signal_level, cell_info) " +
                        "VALUES ($1, $2, $3, to_timestamp($4), $5, $6, $7, $8)";
    
    PGresult* res = PQexecParams(con, query.c_str(), Recvd_size, NULL, 
                                 Recvd_values, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::cerr << "\033[31mОШИБКА ЗАПИСИ\033[0m: " << PQresultErrorMessage(res) << "\n";
    }
    PQclear(res);
}

std::vector<TelemetryData> FetchInitialData() {
    std::vector<TelemetryData> init_list;
    const char* info = "host=" HOST " port=" PORT " dbname=" DB_NAME " user=" DB_USER " password=" DB_USER_PASSWORD;
    PGconn *con = PQconnectdb(info);

    if (PQstatus(con) != CONNECTION_OK) {
        std::cerr << "\033[31mОШИБКА БД (History):\033[0m " << PQerrorMessage(con) << "\n";
        PQfinish(con);
        return init_list; 
    }

    const char* query = "SELECT lat, lon, altitude, extract(epoch from capture_time)::text, "
                        "accuracy, net_type, signal_level, cell_info "
                        "FROM data ORDER BY capture_time DESC LIMIT 500";
    
    PGresult *res = PQexec(con, query);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = rows - 1; i >= 0; i--) { 
            TelemetryData d;
            d.lat = PQgetvalue(res, i, 0);
            d.lon = PQgetvalue(res, i, 1);
            d.alt = PQgetvalue(res, i, 2);
            d.timestamp = PQgetvalue(res, i, 3);
            d.accuracy = PQgetvalue(res, i, 4);
            d.net_type = PQgetvalue(res, i, 5);
            d.signal = PQgetvalue(res, i, 6);
            d.cell_info = PQgetvalue(res, i, 7);
            init_list.push_back(d);
        }
    }
    PQclear(res);
    PQfinish(con);
    return init_list;
}

void RunNetworkModule(SharedBuffer& shared_buffer, std::atomic<bool>& should_run) {
    const char* info = "host=" HOST " port=" PORT " dbname=" DB_NAME " user=" DB_USER " password=" DB_USER_PASSWORD;
    PGconn *con = PQconnectdb(info);

    if (PQstatus(con) != CONNECTION_OK) {
        std::cerr << "\033[31mОШИБКА БД (Network):\033[0m " << PQerrorMessage(con) << "\n";
        PQfinish(con);
        should_run = false;
        return;
    }

    zmq::context_t context(1);
    zmq::socket_t socket(context, zmq::socket_type::rep);
    socket.bind("tcp://*:" ZMQ_PORT);
    socket.set(zmq::sockopt::rcvtimeo, 500);

    std::cout << "Сетевой поток запущен (ZMQ порт: " << ZMQ_PORT << ")\n";

    while (should_run) {
    zmq::message_t request;
    auto res_recv = socket.recv(request, zmq::recv_flags::none);
    if (!res_recv) continue;

    std::string msg = request.to_string();
    std::stringstream ss(msg);
    std::vector<std::string> tokens;
    std::string token;
    
    while (std::getline(ss, token, ';')) {
        tokens.push_back(token);
    }
    if (tokens.size() >= 10) {
    TelemetryData td;
    td.lat = tokens[0];
    td.lon = tokens[1];
    td.alt = tokens[2];
    td.timestamp = tokens[3];
    td.accuracy = tokens[4];
    td.net_type = tokens[5];

    // Синхронизация сигнала с Kotlin (LTE - 13, GSM - 12)
    if (td.net_type == "LTE" && tokens.size() >= 16) td.signal = tokens[15]; // rsrp
    else if (td.net_type == "GSM" && tokens.size() >= 14) td.signal = tokens[13]; // dbm
    else if (td.net_type == "NR" && tokens.size() >= 14) td.signal = tokens[13]; // ssRsrp
    else td.signal = "0";
    td.cell_info = "";
    for (size_t i = 5; i < tokens.size(); ++i) {
        td.cell_info += tokens[i] + (i == tokens.size() - 1 ? "" : ";");
    }

    const char* db_values[] = { 
        td.lat.c_str(), td.lon.c_str(), td.alt.c_str(), 
        td.timestamp.c_str(), td.accuracy.c_str(), 
        td.net_type.c_str(), td.signal.c_str(), td.cell_info.c_str() 
    };
        
        AppendData(con, "data", db_values, 8);
        shared_buffer.addData(td);

        std::string current_flags = shared_buffer.getFlags(); 
        socket.send(zmq::message_t(current_flags.begin(), current_flags.end()), zmq::send_flags::none);
    } else {
        socket.send(zmq::str_buffer("SKIP_EMPTY"), zmq::send_flags::none);
    }
}
    PQfinish(con);
}
