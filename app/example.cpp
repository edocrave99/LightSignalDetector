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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#include <opencv2/imgproc.hpp>
#pragma GCC diagnostic pop
#include <opencv2/video.hpp>
#include <stdlib.h>
#include <syslog.h>
#include <fstream> 

#include "imgprovider.h"

using namespace cv;

int main(void) {
    openlog("opencv_app", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Running OpenCV example with VDO as video source");
    ImgProvider_t* provider = NULL;

    // The desired width and height of the BGR frame
    unsigned int width  = 1920;
    unsigned int height = 1080;

    // Definizione ROI ---
    const int ROI_X = 1613; // Colonna iniziale della ROI
    const int ROI_Y = 657; // Riga iniziale della ROI
    const int ROI_WIDTH = 76; // Larghezza della ROI
    const int ROI_HEIGHT = 141; // Altezza della ROI 

    // Aggiungi soglie minime di pixel per considerare un colore "acceso"
    // Questo valore dovrà essere calibrato in base alle tue condizioni di luce e dimensioni ROI.
    const int MIN_PIXEL_THRESHOLD = 50; // Esempio: almeno 50 pixel per un colore specifico


    syslog(LOG_INFO, "Application started. Desired resolution: %d x %d", width, height);

    // chooseStreamResolution gets the least resource intensive stream
    // that exceeds or equals the desired resolution specified above
    unsigned int streamWidth  = 0;
    unsigned int streamHeight = 0;
    if (!chooseStreamResolution(width, height, &streamWidth, &streamHeight)) {
        syslog(LOG_ERR, "%s: Failed choosing stream resolution", __func__);
        exit(1);
    }

    syslog(LOG_INFO,
           "Creating VDO image provider and creating stream %d x %d",
           streamWidth,
           streamHeight);
    provider = createImgProvider(streamWidth, streamHeight, 2, VDO_FORMAT_YUV);
    if (!provider) {
        syslog(LOG_ERR, "%s: Failed to create ImgProvider", __func__);
        exit(2);
    }

    syslog(LOG_INFO, "Start fetching video frames from VDO");
    if (!startFrameFetch(provider)) {
        syslog(LOG_ERR, "%s: Failed to fetch frames from VDO", __func__);
        exit(3);
    }

    /*
    // Create the background subtractor
    Ptr<BackgroundSubtractorMOG2> bgsub = createBackgroundSubtractorMOG2();

    // Create the filtering element. Its size influences what is considered
    // noise, with a bigger size corresponding to more denoising
    Mat kernel = getStructuringElement(MORPH_ELLIPSE, Size(9, 9));
    */

    
    // Vengono creati 2 contenitori OpenCV Mat per le immagini
    // Uno per il formato BGR e uno per NV12(fornito dal driver video).
    Mat bgr_mat  = Mat(height, width, CV_8UC3);
    Mat nv12_mat = Mat(height * 3 / 2, width, CV_8UC1);
    

    while (true) {

        syslog(LOG_INFO, "--- Requesting new frame ---");

        // Funzione per ottenere l'ultimo frame disponibile
        // E' una funzione bloccante, quindi aspetterà fino a quando un frame non è disponibile
        VdoBuffer* buf = getLastFrameBlocking(provider);
        if (!buf) {
            syslog(LOG_INFO, "No more frames available, exiting");
            exit(0);
        }

        syslog(LOG_INFO, "Frame acquired. BGR frame dimensions: %d x %d", bgr_mat.cols, bgr_mat.rows);

        // Assegna i dati del buffer NV12 all'immagine Mat
        nv12_mat.data = static_cast<uint8_t*>(vdo_buffer_get_data(buf));

        // Converte il frame NV12 in BGR.
        cvtColor(nv12_mat, bgr_mat, COLOR_YUV2BGR_NV12, 3);



        // --- Inizio: Logica di riconoscimento---

        // 1. Assicurarsi che la ROI sia valida
        Rect roi_rect(ROI_X, ROI_Y, ROI_WIDTH, ROI_HEIGHT);
        if (roi_rect.x < 0 || roi_rect.y < 0 ||
            roi_rect.x + roi_rect.width > bgr_mat.cols ||
            roi_rect.y + roi_rect.height > bgr_mat.rows) {
            syslog(LOG_ERR, "ROI is out of image bounds! Skipping frame.");
            continue; // Salta il resto dell'elaborazione per questo frame
        }

        // 2. Crea una "vista" sulla ROI nell'immagine BGR originale
        Mat roi = bgr_mat(roi_rect);
        syslog(LOG_INFO, "Processing ROI at (%d, %d) with size %d x %d",
            roi_rect.x, roi_rect.y, roi_rect.width, roi_rect.height);

        // 3. Converte la ROI in spazio colore HSV
        // HSV è più robusto alle variazioni di illuminazione per il rilevamento del colore.
        Mat hsv_roi;
        cvtColor(roi, hsv_roi, COLOR_BGR2HSV);

        // 4. Definisce i range di colori in HSV per Rosso, Giallo, Verde
        // Questi valori sono GENERALI e DEVONO essere calibrati per le specifiche circostanze.

        // Rosso (Hue è ciclica, quindi il rosso ha due intervalli perchè si trova a cavallo tra l'inizio e la fine del cerchio cromatico)
        Scalar lower_red1 = Scalar(0, 100, 100);
        Scalar upper_red1 = Scalar(10, 255, 255);
        Scalar lower_red2 = Scalar(160, 100, 100);
        Scalar upper_red2 = Scalar(179, 255, 255);

        // Giallo
        Scalar lower_yellow = Scalar(20, 100, 100);
        Scalar upper_yellow = Scalar(30, 255, 255);

        // Verde
        Scalar lower_green = Scalar(50, 100, 100);
        Scalar upper_green = Scalar(70, 255, 255);

        // 5. Crea maschere binarie per ciascun colore all'interno della ROI
        Mat mask_red1, mask_red2, mask_yellow, mask_green;
        inRange(hsv_roi, lower_red1, upper_red1, mask_red1);
        inRange(hsv_roi, lower_red2, upper_red2, mask_red2);
        Mat mask_red;
        bitwise_or(mask_red1, mask_red2, mask_red); // Combina le due maschere rosse

        inRange(hsv_roi, lower_yellow, upper_yellow, mask_yellow);
        inRange(hsv_roi, lower_green, upper_green, mask_green);

        // 6. Conta i pixel non-zero (bianchi) in ciascuna maschera
        // Un conteggio alto indica la presenza significativa del colore.
        int red_pixels = countNonZero(mask_red);
        int yellow_pixels = countNonZero(mask_yellow);
        int green_pixels = countNonZero(mask_green);

        syslog(LOG_INFO, "Pixel counts - Red: %d, Yellow: %d, Green: %d",
                red_pixels, yellow_pixels, green_pixels);

        // 7. Determina lo stato basandosi sul conteggio dei pixel
        std::string state = "UNKNOWN";
        if (red_pixels > MIN_PIXEL_THRESHOLD) {
            state = "RED";
        } else if (yellow_pixels > MIN_PIXEL_THRESHOLD) {
            state = "YELLOW";
        } else if (green_pixels > MIN_PIXEL_THRESHOLD) {
            state = "GREEN";
        }

        /*
        // ALTERNATIVA AL PUNTO 7
        int max_pixels = red_pixels;
        std::string state = "RED";
        if (yellow_pixels > max_pixels) {
            max_pixels = yellow_pixels;
            state = "YELLOW";
        }
        if (green_pixels > max_pixels) {
            max_pixels = green_pixels;
            state = "GREEN";
        }
        if (max_pixels < MIN_PIXEL_THRESHOLD) 
            state = "UNKNOWN";
        */

        syslog(LOG_INFO, "State: %s", state.c_str());

        try {
            // Apri (o crea) un file in /tmp/
            std::ofstream state_file("/tmp/app_state.json"); 
            // Scrivi lo stato in formato JSON
            state_file << "{\"state\": \"" << state << "\"}";
            state_file.close();
        } catch (const std::exception& e) {
            syslog(LOG_ERR, "Failed to write state to file: %s", e.what());
        }
/*
        if (state != "UNKNOWN") {
            // Avvia la funzione in un nuovo thread e lo "sgancia" (detach)
            // per farlo eseguire in background senza bloccare il main.
            std::thread(sendStateToServer, state).detach();
        }
*/
        // --- Fine: Logica di riconoscimento ---


        

        syslog(LOG_INFO, "Frame processing complete. Returning frame to provider.");

        // Release the VDO frame buffer
        returnFrame(provider, buf);
    }
    return EXIT_SUCCESS;
}
