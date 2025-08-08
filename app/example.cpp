// --- LIBRERIE NECESSARIE ---
#include <opencv2/imgproc.hpp> // Per le funzioni di analisi immagine di OpenCV
#include <opencv2/video.hpp>    // Per le funzioni video di OpenCV
#include <stdlib.h>             // Libreria standard
#include <syslog.h>             // Per scrivere messaggi nei log di sistema della telecamera
#include <fstream>              // Per scrivere su file (ofstream)
#include <string>               // Per usare l'oggetto std::string
#include <unistd.h>             // Libreria di sistema UNIX, fondamentale perché contiene la funzione symlink()

#include "imgprovider.h"        // Header per la gestione dei frame video

using namespace cv;

int main(void) {
    // Inizializza il logging, così possiamo vedere i messaggi nel log di sistema
    openlog("opencv_app", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Running OpenCV example with VDO as video source");
    ImgProvider_t* provider = NULL;

    // --- CONFIGURAZIONE ---
    // Definiamo le dimensioni del video e le coordinate della ROI
    unsigned int width  = 1920;
    unsigned int height = 1080;
    const int ROI_X = 1595;
    const int ROI_Y = 759;
    const int ROI_WIDTH = 52;
    const int ROI_HEIGHT = 43;
    const int MIN_PIXEL_THRESHOLD = 40; // Soglia minima di pixel per considerare un colore "presente"

    // Questa è la nostra "scorciatoia". È il file che il browser leggerà.
    // Il percorso è assoluto perché il nostro programma non sa in che cartella si trova quando viene eseguito.
    // "opencv_app" corrisponde al nome dell'applicazione nel manifest.json.
    const char* web_state_file = "/usr/local/packages/opencv_app/html/current_state.txt";


    syslog(LOG_INFO, "Application started. Desired resolution: %d x %d", width, height);

    // --- INIZIALIZZAZIONE VIDEO STREAM ---
    // Qui l'applicazione si connette al flusso video della telecamera
    unsigned int streamWidth  = 0;
    unsigned int streamHeight = 0;
    if (!chooseStreamResolution(width, height, &streamWidth, &streamHeight)) {
        syslog(LOG_ERR, "%s: Failed choosing stream resolution", __func__);
        exit(1);
    }
    provider = createImgProvider(streamWidth, streamHeight, 2, VDO_FORMAT_YUV);
    if (!provider) {
        syslog(LOG_ERR, "%s: Failed to create ImgProvider", __func__);
        exit(2);
    }
    if (!startFrameFetch(provider)) {
        syslog(LOG_ERR, "%s: Failed to fetch frames from VDO", __func__);
        exit(3);
    }

    // --- PREPARAZIONE FRAME OPENCV ---
    // Prepariamo le matrici di OpenCV che conterranno i dati dell'immagine
    Mat bgr_mat  = Mat(height, width, CV_8UC3);
    Mat nv12_mat = Mat(height * 3 / 2, width, CV_8UC1);

    // Variabile per memorizzare l'ultimo stato. Serve per agire solo quando il colore cambia.
    std::string last_written_state = "";

    while (true) {
        // Chiede un nuovo frame video. È una funzione bloccante, aspetta finché non arriva.
        VdoBuffer* buf = getLastFrameBlocking(provider);
        if (!buf) {
            syslog(LOG_INFO, "No more frames available, exiting");
            break;
        }

        // Converte il frame dal formato della telecamera (NV12) al formato BGR usato da OpenCV
        nv12_mat.data = static_cast<uint8_t*>(vdo_buffer_get_data(buf));
        cvtColor(nv12_mat, bgr_mat, COLOR_YUV2BGR_NV12, 3);

        // Seleziona la ROI 
        Rect roi_rect(ROI_X, ROI_Y, ROI_WIDTH, ROI_HEIGHT);
        if (roi_rect.x < 0 || roi_rect.y < 0 ||
            roi_rect.x + roi_rect.width > bgr_mat.cols ||
            roi_rect.y + roi_rect.height > bgr_mat.rows) {
            returnFrame(provider, buf);
            continue;
        }
        Mat roi = bgr_mat(roi_rect);
        
        // --- LOGICA DI RILEVAMENTO COLORE ---
        Mat hsv_roi;
        cvtColor(roi, hsv_roi, COLOR_BGR2HSV);
        Scalar lower_red1 = Scalar(0, 100, 100), upper_red1 = Scalar(10, 255, 255);
        Scalar lower_red2 = Scalar(160, 100, 100), upper_red2 = Scalar(179, 255, 255);
        Scalar lower_yellow = Scalar(20, 100, 100), upper_yellow = Scalar(30, 255, 255);
        Scalar lower_green = Scalar(50, 100, 100), upper_green = Scalar(70, 255, 255);

        Mat mask_red1, mask_red2, mask_yellow, mask_green;
        inRange(hsv_roi, lower_red1, upper_red1, mask_red1);
        inRange(hsv_roi, lower_red2, upper_red2, mask_red2);
        Mat mask_red;
        bitwise_or(mask_red1, mask_red2, mask_red);
        inRange(hsv_roi, lower_yellow, upper_yellow, mask_yellow);
        inRange(hsv_roi, lower_green, upper_green, mask_green);

        int red_pixels = countNonZero(mask_red);
        int yellow_pixels = countNonZero(mask_yellow);
        int green_pixels = countNonZero(mask_green);

        syslog(LOG_INFO, "Pixel counts - Red: %d, Yellow: %d, Green: %d",
                red_pixels, yellow_pixels, green_pixels);

        std::string current_state = "UNKNOWN";
        if (red_pixels > MIN_PIXEL_THRESHOLD) current_state = "RED";
        else if (yellow_pixels > MIN_PIXEL_THRESHOLD) current_state = "YELLOW";
        else if (green_pixels > MIN_PIXEL_THRESHOLD) current_state = "GREEN";

        // --- LOGICA DI AGGIORNAMENTO STATO ("link simbolico") ---
        if (current_state != last_written_state) {
            syslog(LOG_INFO, "State changed to: %s. Updating symlink.", current_state.c_str());
            last_written_state = current_state;

            // 1. CREAZIONE DEL FILE REALE
            // Definiamo un percorso nella cartella /tmp/, che è sempre scrivibile.
            // Il file si chiamerà come lo stato, es. "/tmp/RED.txt".
            std::string temp_state_file_path = "/tmp/" + current_state + ".txt";
            // Creiamo fisicamente il file e ci scriviamo dentro il nome dello stato.
            std::ofstream state_out(temp_state_file_path);
            state_out << current_state;
            state_out.close();

            // 2. AGGIORNAMENTO DEL LINK SIMBOLICO
            // Si rimuove la scorciatoia precedente per evitare errori.
            unlink(web_state_file);
            // Poi creiamo la nuova scorciatoia. La funzione symlink() prende due argomenti:
            // symlink("file_reale", "nome_della_scorciatoia")
            if (symlink(temp_state_file_path.c_str(), web_state_file) != 0) {
                syslog(LOG_ERR, "Failed to create symlink from %s to %s!",
                       temp_state_file_path.c_str(), web_state_file);
            }
        }

        // Rilasciamo il buffer del frame per liberare memoria e permettere l'arrivo del prossimo.
        returnFrame(provider, buf);
    }

    syslog(LOG_INFO, "Application shutting down.");
    return EXIT_SUCCESS;
}