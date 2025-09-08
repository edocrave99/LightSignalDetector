// --- LIBRERIE NECESSARIE ---
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/video.hpp>
#include <stdlib.h>
#include <syslog.h>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <mutex>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <cstdio>

#include "json.hpp"
#include "imgprovider.h"

using namespace cv;

// --- Struttura e Variabili Globali ---
struct AppConfig {
    std::mutex mtx;
    // La risoluzione non è più qui, è fissa
    int master_roi_x = 385, master_roi_y = 207, master_roi_width = 82, master_roi_height = 315;
    int red_x = 42, red_y = 33;
    int yellow_x = 40, yellow_y = 154;
    int green_x = 40, green_y = 251;
    int lamp_radius = 37;
};

AppConfig g_config;
std::atomic<bool> g_reload_config_flag(false);
std::mutex frame_mutex;
std::vector<uchar> jpeg_buffer;

// --- FUNZIONE PER CARICARE LA CONFIGURAZIONE ---
void load_config(const std::string& path) {
    std::ifstream config_file(path);
    if (config_file.good()) {
        try {
            nlohmann::json j = nlohmann::json::parse(config_file);
            std::unique_lock<std::mutex> lock(g_config.mtx);
            // Non si caricano più width/height
            g_config.master_roi_x = j.value("master_roi_x", g_config.master_roi_x);
            g_config.master_roi_y = j.value("master_roi_y", g_config.master_roi_y);
            g_config.master_roi_width = j.value("master_roi_width", g_config.master_roi_width);
            g_config.master_roi_height = j.value("master_roi_height", g_config.master_roi_height);
            g_config.red_x = j.value("red_x", g_config.red_x);
            g_config.red_y = j.value("red_y", g_config.red_y);
            g_config.yellow_x = j.value("yellow_x", g_config.yellow_x);
            g_config.yellow_y = j.value("yellow_y", g_config.yellow_y);
            g_config.green_x = j.value("green_x", g_config.green_x);
            g_config.green_y = j.value("green_y", g_config.green_y);
            g_config.lamp_radius = j.value("lamp_radius", g_config.lamp_radius);
            syslog(LOG_INFO, "Configurazione (ri)caricata da %s", path.c_str());
        } catch (const std::exception& e) {
            syslog(LOG_ERR, "Errore nel parsing del file di configurazione: %s.", e.what());
        }
    }
}

// --- FUNZIONE PER GESTIRE UN SINGOLO CLIENT ---
void HandleClient(int client_sock) {
    char request_buffer[4096] = {0};
    ssize_t bytes_read = read(client_sock, request_buffer, 4095);
    if (bytes_read <= 0) { close(client_sock); return; }
    std::string request(request_buffer, bytes_read);

    // Rimosso l'endpoint /restart
    if (request.find("OPTIONS /save_config") != std::string::npos) {
        std::string response = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: POST, GET, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n\r\n";
        send(client_sock, response.c_str(), response.length(), 0);
    }
    else if (request.find("POST /save_config") != std::string::npos) {
        size_t json_start = request.find("\r\n\r\n");
        if (json_start != std::string::npos) {
            std::string json_body = request.substr(json_start + 4);
            std::string config_path = "/usr/local/packages/opencv_app/html/config.json";
            std::ofstream config_file(config_path);
            config_file << json_body;
            config_file.close();
            chmod(config_path.c_str(), 0644);
            g_reload_config_flag = true;
            std::string response = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"status\":\"success\", \"message\":\"Configurazione salvata e applicata!\"}";
            send(client_sock, response.c_str(), response.length(), 0);
        }
    } 
    else if (request.find("GET /") != std::string::npos) {
        std::string header = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
        send(client_sock, header.c_str(), header.length(), 0);
        while (true) {
            std::vector<uchar> buffer_copy;
            {
                std::unique_lock<std::mutex> lock(frame_mutex);
                if (jpeg_buffer.empty()) { lock.unlock(); usleep(10000); continue; }
                buffer_copy = jpeg_buffer;
            }
            std::string frame_header = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(buffer_copy.size()) + "\r\n\r\n";
            if (send(client_sock, frame_header.c_str(), frame_header.length(), 0) < 0) break;
            if (send(client_sock, buffer_copy.data(), buffer_copy.size(), 0) < 0) break;
            if (send(client_sock, "\r\n", 2, 0) < 0) break;
            usleep(33000);
        }
    }
    close(client_sock);
}

// --- FUNZIONE DEL SERVER MULTITHREAD ---
void MjpegServer(int port) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_sock, 10);
    syslog(LOG_INFO, "Server HTTP/MJPEG in ascolto sulla porta %d", port);

    while (true) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) continue;
        std::thread client_thread(HandleClient, client_sock);
        client_thread.detach();
    }
}

// --- FUNZIONE PRINCIPALE ---
int main(void) {
    openlog("opencv_app", LOG_PID | LOG_CONS, LOG_USER);
    int server_port = 8080;
    std::thread server_thread(MjpegServer, server_port);
    server_thread.detach();

    const int MIN_BRIGHTNESS_THRESHOLD = 80;
    std::string config_path = "/usr/local/packages/opencv_app/html/config.json";
    
    // Caricamento iniziale della configurazione
    load_config(config_path);
    
    // Risoluzione fissa
    const unsigned int width = 1280;
    const unsigned int height = 720;

    syslog(LOG_INFO, "Avvio dello stream a risoluzione fissa: %dx%d", width, height);
    ImgProvider_t* provider = createImgProvider(width, height, 2, VDO_FORMAT_RGB);
    if (!provider || !startFrameFetch(provider)) {
        syslog(LOG_ERR, "FALLIMENTO: Impossibile avviare lo stream a %dx%d.", width, height);
        exit(1);
    }
    
    Mat rgb_mat(height, width, CV_8UC3);
    Mat bgr_mat(height, width, CV_8UC3);

    while (true) {
        if (g_reload_config_flag) {
            load_config(config_path);
            g_reload_config_flag = false;
        }

        AppConfig current_config;
        {
            std::unique_lock<std::mutex> lock(g_config.mtx);
            current_config.master_roi_x = g_config.master_roi_x;
            current_config.master_roi_y = g_config.master_roi_y;
            current_config.master_roi_width = g_config.master_roi_width;
            current_config.master_roi_height = g_config.master_roi_height;
            current_config.red_x = g_config.red_x;
            current_config.red_y = g_config.red_y;
            current_config.yellow_x = g_config.yellow_x;
            current_config.yellow_y = g_config.yellow_y;
            current_config.green_x = g_config.green_x;
            current_config.green_y = g_config.green_y;
            current_config.lamp_radius = g_config.lamp_radius;
        }

        VdoBuffer* buf = getLastFrameBlocking(provider);
        if (!buf) {
            syslog(LOG_ERR, "Stream video interrotto!");
            break;
        }
        
        rgb_mat.data = static_cast<uint8_t*>(vdo_buffer_get_data(buf));
        cvtColor(rgb_mat, bgr_mat, COLOR_RGB2BGR);

        Rect master_roi_rect(current_config.master_roi_x, current_config.master_roi_y, current_config.master_roi_width, current_config.master_roi_height);
        
        if (master_roi_rect.width <= 0 || master_roi_rect.height <= 0 ||
            master_roi_rect.x < 0 || master_roi_rect.y < 0 ||
            master_roi_rect.x + master_roi_rect.width > bgr_mat.cols ||
            master_roi_rect.y + master_roi_rect.height > bgr_mat.rows) {
            circle(bgr_mat, Point(30, 30), 20, Scalar(128, 128, 128), -1);
        } else {
            Mat cropped_bgr = bgr_mat(master_roi_rect);
            std::vector<Point> lamp_centers;
            lamp_centers.push_back(Point(current_config.red_x, current_config.red_y));
            lamp_centers.push_back(Point(current_config.yellow_x, current_config.yellow_y));
            lamp_centers.push_back(Point(current_config.green_x, current_config.green_y));
            
            std::vector<Mat> bgr_planes;
            split(cropped_bgr, bgr_planes);
            Mat green_plane = bgr_planes[1];
            Mat red_plane = bgr_planes[2];
            std::vector<Mat> planes_to_check = {red_plane, red_plane, green_plane};
            double max_avg_brightness = 0.0;
            int brightest_idx = -1;
            for (size_t i = 0; i < lamp_centers.size(); ++i) {
                if (lamp_centers[i].x < 0 || lamp_centers[i].y < 0 || lamp_centers[i].x >= cropped_bgr.cols || lamp_centers[i].y >= cropped_bgr.rows) {
                    continue;
                }
                Mat mask = Mat::zeros(cropped_bgr.size(), CV_8UC1);
                circle(mask, lamp_centers[i], current_config.lamp_radius, Scalar(255), FILLED);
                Scalar avg_scalar = mean(planes_to_check[i], mask);
                double current_avg_brightness = avg_scalar[0];
                if (current_avg_brightness > max_avg_brightness) {
                    max_avg_brightness = current_avg_brightness;
                    brightest_idx = i;
                }
            }
            std::string current_state = "UNKNOWN";
            if (brightest_idx != -1 && max_avg_brightness > MIN_BRIGHTNESS_THRESHOLD) {
                if (brightest_idx == 0) current_state = "RED";
                else if (brightest_idx == 1) current_state = "YELLOW";
                else if (brightest_idx == 2) current_state = "GREEN";
            }
            Point circle_center(30, 30);
            int circle_radius = 20;
            Scalar circle_color;
            if (current_state == "RED") circle_color = Scalar(0, 0, 255);
            else if (current_state == "YELLOW") circle_color = Scalar(0, 255, 255);
            else if (current_state == "GREEN") circle_color = Scalar(0, 255, 0);
            else circle_color = Scalar(128, 128, 128);
            circle(bgr_mat, circle_center, circle_radius, circle_color, -1);
        }
        
        std::vector<int> params;
        params.push_back(IMWRITE_JPEG_QUALITY);
        params.push_back(75);
        std::vector<uchar> temp_jpeg_buffer;
        imencode(".jpg", bgr_mat, temp_jpeg_buffer, params);
        {
            std::unique_lock<std::mutex> lock(frame_mutex);
            jpeg_buffer = temp_jpeg_buffer;
        }
        
        returnFrame(provider, buf);
    }

    syslog(LOG_INFO, "Application shutting down.");
    return EXIT_SUCCESS;
}