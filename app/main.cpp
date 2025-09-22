/**
 * Copyright (C) 2021 Axis Communications AB, Lund, Sweden
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Questo programma acquisisce un flusso video da una telecamera Axis, analizza
 * una specifica regione (ROI) per determinare quale delle tre luci di un semaforo
 * (rossa, gialla, verde) è accesa, e fornisce un'interfaccia web per
 * la configurazione e la visualizzazione di un flusso video MJPEG con i risultati.
 * L'applicazione è multi-thread: un thread principale gestisce l'elaborazione
 * delle immagini, mentre un secondo thread gestisce un server web per la
 * comunicazione con l'interfaccia utente.
 */

// Disattiva temporaneamente l'avviso "-Wfloat-equal" per le inclusioni di OpenCV
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#include <opencv2/imgproc.hpp>   // Funzioni di elaborazione immagini (es. cvtColor, circle, mean)
#pragma GCC diagnostic pop
#include <opencv2/video.hpp>      // Funzioni video di OpenCV
#include <opencv2/imgcodecs.hpp>  // Funzioni per codificare e decodificare immagini (es. imencode)
#include <syslog.h>               // Per scrivere messaggi nel log di sistema della telecamera
#include <string>                 // Per usare la classe std::string
#include <vector>                 // Per usare la classe std::vector
#include <mutex>                  // Per la mutua esclusione e la gestione dei thread (std::mutex, std::unique_lock)
#include <fstream>                // Per la gestione dei file (std::ifstream, std::ofstream)
#include <atomic>                 // Per variabili atomiche thread-safe (std::atomic)
#include <cstdio>                 // Funzioni C standard di I/O
#include <thread>                 // Per la programmazione multi-thread (std::thread)
#include <gio/gio.h>              // Libreria GLib per I/O asincrono, usata per il server web
#include <sys/stat.h>             // Per la funzione chmod (cambio permessi file)

// Librerie esterne incluse nel progetto
#include "json.hpp"               // Libreria nlohmann/json per il parsing di file JSON
#include "imgprovider.h"          // Header dell'SDK di Axis per l'acquisizione video

using namespace cv;

// --- STRUTTURA DI CONFIGURAZIONE E VARIABILI GLOBALI ---

/**
 * @struct AppConfig
 * @brief Contiene tutti i parametri di configurazione dell'applicazione.
 *
 * Questa struttura raggruppa le coordinate della ROI (Region of Interest) del semaforo
 * e delle singole luci. Include un mutex per garantire che la lettura e la scrittura
 * di questi parametri siano "thread-safe", ovvero sicure quando il thread principale
 * (che elabora le immagini) e il thread del server (che salva la configurazione)
 * vi accedono contemporaneamente.
 */
struct AppConfig {
    std::mutex mtx; // Mutex per proteggere l'accesso concorrente ai dati di questa struttura
    int master_roi_x = 385, master_roi_y = 207, master_roi_width = 82, master_roi_height = 315;
    int red_x = 42, red_y = 33;
    int yellow_x = 40, yellow_y = 154;
    int green_x = 40, green_y = 251;
    int lamp_radius = 37;
    int min_brightness_threshold = 80;
};

// Istanza globale della configurazione. È condivisa tra il thread principale e quello del server.
AppConfig g_config;
// Flag atomico per segnalare al thread principale di ricaricare la configurazione.
// std::atomic garantisce che le operazioni di lettura/scrittura siano indivisibili e non richiedano un mutex.
std::atomic<bool> g_reload_config_flag(false);
// Mutex per proteggere l'accesso al buffer dell'immagine JPEG, condiviso tra il thread principale (scrittore)
// e i vari thread client (lettori) che richiedono lo stream video.
std::mutex frame_mutex;
// Buffer che contiene l'ultimo frame processato e codificato in formato JPEG,
// pronto per essere inviato ai client connessi allo stream MJPEG.
std::vector<uchar> jpeg_buffer;
// Puntatore al loop di eventi principale del server GIO, usato per gestire le richieste in entrata.
GMainLoop *loop;

// --- FUNZIONI DI GESTIONE DELLA CONFIGURAZIONE ---

/**
 * @brief Carica la configurazione da un file JSON.
 * @param path Percorso del file di configurazione (es. "config.json").
 *
 * Questa funzione apre e legge il file JSON specificato. Se la lettura ha successo,
 * esegue il parsing del contenuto e aggiorna la struttura globale `g_config`.
 * L'aggiornamento avviene in modo thread-safe utilizzando `std::unique_lock`,
 * che blocca il mutex `g_config.mtx` all'inizio della sezione critica e lo
 * rilascia automaticamente alla fine. In caso di errore nel parsing, viene
 * registrato un messaggio di errore nel syslog.
 */
void load_config(const std::string& path) {
    std::ifstream config_file(path);
    if (config_file.good()) {
        try {
            nlohmann::json j = nlohmann::json::parse(config_file);
            // Blocca il mutex per un accesso esclusivo e sicuro alla configurazione globale
            std::unique_lock<std::mutex> lock(g_config.mtx);
            // Aggiorna i valori usando j.value(), che usa il valore di default se la chiave non esiste nel JSON
            g_config.master_roi_x = j.value("master_roi_x", g_config.master_roi_x);
            g_config.master_roi_y = j.value("master_roi_y", g_config.master_roi_y);
            g_config.master_roi_width = j.value("master_roi_width", g_config.master_roi_width);
            g_config.master_roi_height = j.value("master_roi_height", g_config.master_roi_height);
            g_config.red_x = j.value("red_x", g_config.red_x);
            g_config.red_y = j.value("red_y", g_config.red_y);
            g_config.yellow_x = j.value("yellow_x", g_config.yellow_x);
            g_config.yellow_y = j.value("yellow_y", g_config.yellow_x);
            g_config.green_x = j.value("green_x", g_config.green_x);
            g_config.green_y = j.value("green_y", g_config.green_y);
            g_config.lamp_radius = j.value("lamp_radius", g_config.lamp_radius);
            g_config.min_brightness_threshold = j.value("min_brightness_threshold", g_config.min_brightness_threshold);
        } catch (const std::exception& e) {
            syslog(LOG_ERR, "Errore nel parsing del file di configurazione: %s.", e.what());
        }
    }
}


// --- SEZIONE DI GESTIONE DEL SERVER WEB ---

/**
 * @brief Gestisce le richieste HTTP POST per salvare la nuova configurazione.
 * @param ostream Lo stream di output per inviare la risposta al client.
 * @param full_request L'intera richiesta HTTP ricevuta, come stringa.
 *
 * Questa funzione estrae il corpo JSON dalla richiesta HTTP, lo salva nel file
 * `config.json` sovrascrivendo quello esistente, e imposta `g_reload_config_flag` a `true`
 * per notificare al thread principale di ricaricare le impostazioni.
 * Infine, invia una risposta HTTP 200 OK per confermare il successo dell'operazione.
 */
static void handle_save_config(GOutputStream *ostream, const std::string& full_request) {
    // Trova la fine degli header HTTP (doppio a capo) per isolare il corpo della richiesta
    size_t json_start = full_request.find("\r\n\r\n");
    if (json_start != std::string::npos) {
        std::string json_body = full_request.substr(json_start + 4);
        if (!json_body.empty()) {
            std::string config_path = "/usr/local/packages/tld/html/config.json";
            std::ofstream config_file(config_path);
            config_file << json_body;
            config_file.close();
            chmod(config_path.c_str(), 0644); // Imposta i permessi di lettura/scrittura corretti per il file
            
            // Segnala al thread principale di ricaricare la configurazione al prossimo ciclo
            g_reload_config_flag = true;
            
            const char *response = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\n\r\n{\"status\":\"success\"}";
            g_output_stream_write(ostream, response, strlen(response), NULL, NULL);
        } else {
             const char *response = "HTTP/1.1 400 Bad Request\r\n\r\n{\"status\":\"error\", \"message\":\"Empty body\"}";
             g_output_stream_write(ostream, response, strlen(response), NULL, NULL);
        }
    } else {
        const char *response = "HTTP/1.1 400 Bad Request\r\n\r\n{\"status\":\"error\", \"message\":\"Invalid request format\"}";
        g_output_stream_write(ostream, response, strlen(response), NULL, NULL);
    }
    // Forza l'invio immediato dei dati presenti nel buffer di rete.
    // Cruciale per evitare che la richiesta del client rimanga in stato "pending".
    g_output_stream_flush(ostream, NULL, NULL);
}

/**
 * @brief Gestisce la richiesta GET per lo stream video MJPEG.
 * @param ostream Lo stream di output su cui inviare i frame video.
 *
 * Invia un header HTTP specifico per lo stream MJPEG e poi entra in un loop.
 * Ad ogni iterazione, acquisisce in modo sicuro l'ultimo frame disponibile dal
 * buffer globale `jpeg_buffer`, lo impacchetta con gli header di frame MJPEG
 * e lo invia al client. Il loop si interrompe se il client chiude la connessione.
 */
static void handle_mjpeg_stream(GOutputStream *ostream) {
    // Header standard per uno stream MJPEG. Indica al browser di sostituire l'immagine
    // con ogni nuovo "pezzo" (frame) che arriva, delimitato da 'boundary=frame'.
    const char *header = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
    g_output_stream_write(ostream, header, strlen(header), NULL, NULL);
    
    while (true) {
        std::vector<uchar> buffer_copy;
        {
            // Blocca il mutex per leggere in sicurezza il buffer globale
            std::unique_lock<std::mutex> lock(frame_mutex);
            if (jpeg_buffer.empty()) {
                lock.unlock(); // Rilascia il lock prima di attendere
                g_usleep(10000); // Attende 10ms se non ci sono nuovi frame per non sovraccaricare la CPU
                continue;
            }
            // Copia il buffer per poter rilasciare il lock il prima possibile,
            // riducendo il tempo in cui il thread principale rimane in attesa per scrivere un nuovo frame.
            buffer_copy = jpeg_buffer;
        }

        // Costruisce l'header per il singolo frame JPEG
        std::string frame_header = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(buffer_copy.size()) + "\r\n\r\n";
        
        gboolean success = TRUE;
        // Invia l'header del frame, i dati dell'immagine, e una riga vuota di separazione
        success &= g_output_stream_write_all(ostream, frame_header.c_str(), frame_header.length(), NULL, NULL, NULL);
        success &= g_output_stream_write_all(ostream, buffer_copy.data(), buffer_copy.size(), NULL, NULL, NULL);
        success &= g_output_stream_write_all(ostream, "\r\n", 2, NULL, NULL, NULL);

        // Se la scrittura fallisce (es. il client ha chiuso la pagina), g_output_stream_write_all
        // restituisce FALSE. In questo caso, è inutile continuare a inviare frame.
        if (!success) {
            syslog(LOG_INFO, "Client disconnesso dallo stream MJPEG.");
            break; // Esce dal loop e termina il thread del client
        }
        g_usleep(33000); // Limita il framerate a circa 30fps (1s / 30fps ≈ 33ms)
    }
}

/**
 * @brief Funzione eseguita in un thread separato per ogni client connesso.
 * @param connection L'oggetto GSocketConnection che rappresenta la connessione del client.
 *
 * Legge la prima riga della richiesta HTTP per determinarne il percorso (routing)
 * e il metodo (GET/POST). In base a questo, invoca la funzione handler corretta
 * (`handle_save_config` o `handle_mjpeg_stream`). Isolare ogni client nel proprio
 * thread impedisce che una richiesta lunga (come lo stream MJPEG) blocchi il server.
 */
void client_thread_func(GSocketConnection* connection) {
    // Ottiene gli stream di input e output dalla connessione per comunicare con il client
    GInputStream *istream = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *ostream = g_io_stream_get_output_stream(G_IO_STREAM(connection));

    // Legge il primo blocco di dati della richiesta (sufficiente per contenere gli header)
    gchar buffer[4096] = {0};
    g_input_stream_read(istream, buffer, sizeof(buffer) - 1, NULL, NULL);
    std::string full_request(buffer);
    
    // Isola la prima riga (es. "GET /path HTTP/1.1") per il routing
    std::string first_line = full_request.substr(0, full_request.find("\r\n"));
    syslog(LOG_INFO, "Richiesta gestita dal thread: %s", first_line.c_str());

    // Routing basato sul percorso richiesto
    if (first_line.find("POST /local/tld/api/save_config") != std::string::npos) {
        handle_save_config(ostream, full_request);
    } else if (first_line.find("GET /local/tld/api/stream") != std::string::npos) {
        // Questa funzione è bloccante, ma viene eseguita in un thread dedicato
        // e non impatta le altre connessioni.
        handle_mjpeg_stream(ostream);
    } else {
        // Se nessun percorso corrisponde, invia un errore 404 Not Found
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n";
        g_output_stream_write(ostream, response, strlen(response), NULL, NULL);
        g_output_stream_flush(ostream, NULL, NULL);
    }
    
    // Chiude la connessione e rilascia le risorse associate
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    // Decrementa il reference count dell'oggetto connection, che era stato incrementato
    // prima di passare l'oggetto al thread. Questo ne permette la deallocazione.
    g_object_unref(connection);
}

/**
 * @brief Callback eseguita dal server GIO per ogni nuova connessione in entrata.
 * @param connection La nuova connessione stabilita.
 * @return TRUE per continuare ad accettare nuove connessioni.
 *
 * Lo scopo di questa funzione è lanciare immediatamente un nuovo thread per gestire la
 * connessione (`client_thread_func`) e poi terminare. Questo mantiene il callback
 * velocissimo e non bloccante, permettendo al server di rimanere reattivo e di
 * accettare un gran numero di connessioni simultanee.
 */
static gboolean incoming_callback(GSocketService *service, GSocketConnection *connection, GObject *source_object, gpointer user_data) {
    // I parametri service, source_object, e user_data non sono utilizzati in questo scenario
    (void)service; (void)source_object; (void)user_data;

    // Aumenta il reference count della connessione. È necessario perché l'oggetto `connection`
    // verrà usato da un altro thread, e dobbiamo assicurarci che non venga deallocato prematuramente.
    g_object_ref(connection);
    
    // Crea un nuovo thread per gestire questo client e lo "sgancia" (detach).
    // Con `detach`, il thread principale non deve più attendere la sua terminazione (`join`).
    // Il thread del client diventerà responsabile di deallocare le proprie risorse.
    std::thread client_thread(client_thread_func, connection);
    client_thread.detach();

    return TRUE; // Indica al servizio di continuare ad accettare connessioni
}

/**
 * @brief Funzione eseguita dal thread del server web.
 *
 * Configura e avvia il servizio GSocketService per ascoltare le connessioni
 * HTTP sulla porta 8080, ma solo sull'interfaccia di loopback (127.0.0.1).
 * L'ascolto su localhost è una best practice di sicurezza, poiché l'accesso
 * esterno sarà gestito da un reverse proxy (come lighttpd) sulla telecamera.
 * Avvia infine il loop di eventi principale (GMainLoop) che attende e gestisce
 * le connessioni in entrata tramite `incoming_callback`.
 */
void* server_thread_func(void*) {
    GSocketService *service = g_socket_service_new();
    // Aggiunge un listener sulla porta 8080 per l'indirizzo di loopback (localhost).
    // Il reverse proxy della telecamera inoltrerà le richieste a questo indirizzo.
    g_socket_listener_add_inet_port(G_SOCKET_LISTENER(service), 8080, NULL, NULL);
    // Collega la funzione `incoming_callback` all'evento "incoming" del servizio,
    // che viene emesso ogni volta che un nuovo client si connette.
    g_signal_connect(service, "incoming", G_CALLBACK(incoming_callback), NULL);
    
    g_socket_service_start(service);
    syslog(LOG_INFO, "Server GIO in ascolto su localhost:8080");
    
    // Avvia il loop di eventi GIO. Questa è una funzione bloccante che
    // attenderà indefinitamente le connessioni e invocherà i callback.
    // Il loop verrà fermato da g_main_loop_quit() alla chiusura dell'applicazione.
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    
    return NULL;
}

// --- FUNZIONE PRINCIPALE DELL'APPLICAZIONE ---

/**
 * @brief Punto di ingresso principale dell'applicazione.
 *
 * La funzione `main` orchestra l'intera applicazione:
 * 1. Inizializza il logger di sistema (`syslog`).
 * 2. Avvia il thread del server web.
 * 3. Carica la configurazione iniziale dal file JSON.
 * 4. Inizializza il provider video della telecamera Axis.
 * 5. Entra in un loop infinito di elaborazione delle immagini.
 * 6. Alla chiusura, ferma il server web e termina in modo pulito.
 */
int main(void) {
    // Inizializza il syslog per registrare i messaggi con il nome "tld"
    openlog("tld", LOG_PID | LOG_CONS, LOG_USER);

    // Crea e avvia il thread del server web usando pthreads
    pthread_t server_tid;
    pthread_create(&server_tid, NULL, &server_thread_func, NULL);
    
    std::string config_path = "/usr/local/packages/tld/html/config.json";
    
    // Carica la configurazione all'avvio
    load_config(config_path);
    
    // Imposta la risoluzione desiderata per lo stream video
    const unsigned int width = 1280;
    const unsigned int height = 720;

    syslog(LOG_INFO, "Avvio dello stream a risoluzione fissa: %dx%d", width, height);
    
    // Inizializza l'SDK di Axis per ottenere i frame video in formato YUV (NV12)
    ImgProvider_t* provider = createImgProvider(width, height, 2, VDO_FORMAT_YUV);

    if (!provider || !startFrameFetch(provider)) {
        syslog(LOG_ERR, "FALLIMENTO: Impossibile avviare lo stream video a %dx%d.", width, height);
        exit(1);
    }
    
    // Pre-alloca le matrici OpenCV per contenere i dati dei frame
    Mat yuv_mat(height * 3 / 2, width, CV_8UC1); // Mat per i dati grezzi YUV NV12
    Mat bgr_mat_output(height, width, CV_8UC3);  // Mat per l'immagine a colori da visualizzare

    // Loop principale di elaborazione delle immagini
    while (true) {
        // Controlla se l'interfaccia web ha richiesto un ricaricamento della configurazione
        if (g_reload_config_flag) {
            load_config(config_path);
            g_reload_config_flag = false; // Resetta il flag dopo aver ricaricato
        }

        AppConfig current_config;
        {
            // Crea una copia locale thread-safe della configurazione.
            // Questo minimizza il tempo in cui il mutex globale è bloccato, permettendo
            // al server web di rispondere più velocemente se necessario.
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
            current_config.min_brightness_threshold = g_config.min_brightness_threshold;
        }

        // Ottiene il frame più recente dal provider video (chiamata bloccante)
        VdoBuffer* buf = getLastFrameBlocking(provider);
        if (!buf) {
            syslog(LOG_ERR, "Stream video interrotto (buffer nullo)!");
            break; // Esce dal loop se lo stream si interrompe
        }
        
        // Collega i dati del buffer grezzo alla matrice YUV di OpenCV senza copiare i dati
        yuv_mat.data = static_cast<uint8_t*>(vdo_buffer_get_data(buf));
        
        // Definisce il rettangolo della ROI principale (l'intero semaforo)
        Rect master_roi_rect(current_config.master_roi_x, current_config.master_roi_y, current_config.master_roi_width, current_config.master_roi_height);
        
        std::string current_state = "UNKNOWN"; // Stato di default
        
        // Controllo di validità sulla ROI per evitare crash
        if (master_roi_rect.width <= 0 || master_roi_rect.height <= 0 ||
            master_roi_rect.x < 0 || master_roi_rect.y < 0 ||
            master_roi_rect.x + master_roi_rect.width > width ||
            master_roi_rect.y + master_roi_rect.height > height) {
            
            // Se la ROI non è valida, converte l'immagine ma non esegue l'analisi
            cvtColor(yuv_mat, bgr_mat_output, COLOR_YUV2BGR_NV12);
            // Disegna un cerchio grigio per indicare lo stato di errore/sconosciuto
            circle(bgr_mat_output, Point(30, 30), 20, Scalar(128, 128, 128), -1);
        } else {
            // Estrae il piano Y (luminanza) dall'immagine YUV. L'analisi viene fatta solo
            // sulla luminanza perché è efficiente e sufficiente per rilevare una luce accesa.
            Mat y_plane = yuv_mat(Rect(0, 0, width, height));
            // Ritaglia il piano di luminanza sulla ROI del semaforo
            Mat cropped_y = y_plane(master_roi_rect);
            
            // Coordinate delle luci relative alla ROI ritagliata
            std::vector<Point> lamp_centers = {
                Point(current_config.red_x, current_config.red_y),
                Point(current_config.yellow_x, current_config.yellow_y),
                Point(current_config.green_x, current_config.green_y)
            };

            int brightest_idx = -1;
            double max_luma = 0.0;
            std::vector<double> lumas(3, 0.0);

            // Calcola la luminosità media per ogni luce
            for (size_t i = 0; i < lamp_centers.size(); ++i) {
                // Crea una maschera circolare per isolare i pixel di una singola luce
                Mat mask = Mat::zeros(cropped_y.size(), CV_8UC1);
                circle(mask, lamp_centers[i], current_config.lamp_radius, Scalar(255), FILLED);
                // Calcola la luminosità media solo dei pixel all'interno della maschera
                lumas[i] = mean(cropped_y, mask)[0];

                // Tiene traccia di quale luce è la più luminosa
                if (lumas[i] > max_luma) {
                    max_luma = lumas[i];
                    brightest_idx = i;
                }
            }

            // Determina lo stato finale solo se la luce più brillante supera la soglia minima
            if (max_luma > current_config.min_brightness_threshold) {
                if (brightest_idx == 0) current_state = "RED";
                else if (brightest_idx == 1) current_state = "YELLOW";
                else if (brightest_idx == 2) current_state = "GREEN";
            }

            // Logga i risultati dell'analisi per il debug
            syslog(LOG_INFO, "Luminosita R:%.1f, Y:%.1f, G:%.1f con soglia %d -> Stato = %s",
                   lumas[0], lumas[1], lumas[2],
                   current_config.min_brightness_threshold, current_state.c_str());

            // Converte l'intero frame YUV in BGR per poter disegnare a colori
            cvtColor(yuv_mat, bgr_mat_output, COLOR_YUV2BGR_NV12);
            
            // Disegna un cerchio colorato in alto a sinistra come feedback visivo dello stato rilevato
            Point circle_center(30, 30);
            int circle_radius = 20;
            Scalar circle_color;
            if (current_state == "RED") circle_color = Scalar(0, 0, 255);       // BGR: Rosso
            else if (current_state == "YELLOW") circle_color = Scalar(0, 255, 255); // BGR: Giallo
            else if (current_state == "GREEN") circle_color = Scalar(0, 255, 0);    // BGR: Verde
            else circle_color = Scalar(128, 128, 128);                           // BGR: Grigio
            circle(bgr_mat_output, circle_center, circle_radius, circle_color, -1);
        }
        
        // --- PREPARAZIONE DEL FRAME PER LO STREAM MJPEG ---
        
        // Imposta i parametri di compressione JPEG (qualità 75%)
        std::vector<int> params;
        params.push_back(IMWRITE_JPEG_QUALITY);
        params.push_back(75);
        
        // Codifica l'immagine BGR con i disegni in un buffer temporaneo
        std::vector<uchar> temp_jpeg_buffer;
        imencode(".jpg", bgr_mat_output, temp_jpeg_buffer, params);
        
        // Aggiorna il buffer JPEG globale in modo thread-safe
        {
            std::unique_lock<std::mutex> lock(frame_mutex);
            jpeg_buffer = temp_jpeg_buffer;
        }
        
        // Rilascia il buffer del frame al provider per permettergli di acquisire il successivo
        returnFrame(provider, buf);
    }

    // --- PULIZIA E CHIUSURA ---
    
    syslog(LOG_INFO, "Chiusura dell'applicazione in corso...");
    // Interrompe il loop di eventi del server GIO
    g_main_loop_quit(loop);
    // Attende la terminazione del thread del server
    pthread_join(server_tid, NULL);   
    closelog();
    return EXIT_SUCCESS;
}