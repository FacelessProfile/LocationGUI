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
#define DB_NAME "YourDbName"
#define DB_USER "YourUser"
#define DB_USER_PASSWORD "YourPassword"
#define ZMQ_PORT "YourPort"

void AppendData(PGconn *con, const std::string& table, const char **Recvd_values, const int Recvd_size) {
    std::string query = "INSERT INTO " + table +
                        "(lat, lon, signal_level, capture_time) " +
                        "VALUES ($1, $2, $3, to_timestamp($4))";
    
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

    const char* query = "SELECT lat, lon, signal_level, extract(epoch from capture_time)::text "
                        "FROM data ORDER BY capture_time DESC LIMIT 500";
    
    PGresult *res = PQexec(con, query);

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = rows - 1; i >= 0; i--) { 
            TelemetryData d;
            d.lat = PQgetvalue(res, i, 0);
            d.lon = PQgetvalue(res, i, 1);
            d.signal = PQgetvalue(res, i, 2);
            d.timestamp = PQgetvalue(res, i, 3);
            init_list.push_back(d);
        }
        std::cout << "Загружено " << rows << " точек истории из БД.\n";
    } else {
        std::cerr << "Ошибка запроса истории: " << PQresultErrorMessage(res) << "\n";
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

    int timeout_ms = 500;
    socket.set(zmq::sockopt::rcvtimeo, timeout_ms);

    std::cout << "Сетевой поток запущен (ZMQ порт: " << ZMQ_PORT << ")\n";

    while (should_run) {
        zmq::message_t request;
        auto res_recv = socket.recv(request, zmq::recv_flags::none);
        if (!res_recv) continue;

        std::string msg = request.to_string();
        std::stringstream ss(msg);
        std::string lat, lon, signal, timestamp;
        if (std::getline(ss, lat, ';') &&
            std::getline(ss, lon, ';') &&
            std::getline(ss, signal, ';') &&
            std::getline(ss, timestamp, ';')) 
        {
            const char* values[] = { lat.c_str(), lon.c_str(), signal.c_str(), timestamp.c_str() };
            AppendData(con, "data", values, 4);
            TelemetryData td = { lat, lon, signal, timestamp };
            shared_buffer.addData(td);
            socket.send(zmq::str_buffer("ACK"), zmq::send_flags::none);
        } 
        else {
            std::cerr << "Получены некорректные данные: " << msg << "\n";
            socket.send(zmq::str_buffer("ERROR"), zmq::send_flags::none);
        }
    }
    PQfinish(con);
    std::cout << "Сетевой поток остановлен.\n";
}
