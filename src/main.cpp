#include <iostream>
#include <thread>
#include <atomic>
#include <vector>

#include "headers/data.h"
void RunGui(SharedBuffer& shared_data, std::atomic<bool>& running);
void RunNetworkModule(SharedBuffer& shared_buffer, std::atomic<bool>& should_run);
std::vector<TelemetryData> FetchInitialData();

int main(int argc, char* argv[]) {
    SharedBuffer sharedData;
    std::atomic<bool> running(true);

    std::cout << "--------------START------------" << std::endl;
    std::cout << "[БД] Загрузка последних 500 точек" << std::endl;
    std::vector<TelemetryData> history = FetchInitialData(); 
    
    if (!history.empty()) {
        sharedData.setInitialHistory(history);
        std::cout << "[БД] Точки загружены" << std::endl;
    } else {
        std::cout << "[БД] История пуста или база недоступна" << std::endl;
    }
    std::cout << "[СЕТЬ] Запуск потока ZMQ..." << std::endl;
    std::thread netThread(RunNetworkModule, std::ref(sharedData), std::ref(running));
    std::cout << "[GUI] Инициализация интерфейса..." << std::endl;
    try {
        RunGui(sharedData, running);
    } catch (const std::exception& e) {
        std::cerr << "[ERR] Ошибка в GUI: " << e.what() << std::endl;
        running = false;
    }
    std::cout << "------------SHUTDOWN--------------" << std::endl;
    running = false;
    if (netThread.joinable()) {
        netThread.join();
        std::cout << "[СЕТЬ] Поток успешно остановлен" << std::endl;
    }

    return 0;
}
