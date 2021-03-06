/*
 * app_httpd.hpp
 *
 *  Created on: Apr 4, 2019
 *      Author: andri
 */

#ifndef MAIN_APP_HTTPD_HPP_
#define MAIN_APP_HTTPD_HPP_

#include <esp_log.h>
#include "esp_http_server.h"
#include "sdkconfig.h"
#include "CustomVisionClient.h"
#include "secrets.h"
#include "DXWiFi.h"

#if CONFIG_CAMERA_BOARD_TTGO_TCAM
#include "ssd1306.h"
#endif

#define TAG_HTTPD "HTTPD"

#define STREAM_PREDICTION_INTERVAL 	10000000 //10 seconds
#define MIN_PROB_BEST_PREDICTION 	0.6f

// For streaming-related
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char* DETECTING_MESSAGE = "Recognizing...";
static const char* DETECTING2_MESSAGE = "Detecting..";
static const char* DETECT_NO_RESULT_MESSAGE = "Nobody there :(";

typedef enum
{
    APP_BEGAN,
	APP_WAIT_FOR_CONNECT,
	APP_WIFI_CONNECTED,
	APP_START_DETECT,
	APP_START_RECOGNITION,
	APP_DONE_RECOGNITION,
	APP_START_ENROLL,
	APP_START_DELETE,

} app_state_e;

httpd_handle_t camera_httpd = NULL;

app_state_e app_state = APP_BEGAN;

static esp_err_t index_handler(httpd_req_t *req){
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "hello", 5);
}

static esp_err_t capture_handler(httpd_req_t *req){
	esp_err_t res = ESP_OK;

    camera_fb_t *fb = esp_camera_fb_get();
	if (!fb) {
		ESP_LOGE(TAG_HTTPD, "Camera buffer is not accessible");
		httpd_resp_send_500(req);
		res = ESP_FAIL;
	}
	else {
		if (fb->format == PIXFORMAT_JPEG) {
			httpd_resp_set_type(req, "image/jpeg");
			httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

			res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
		}
	}
	esp_camera_fb_return(fb);

	return res;
}

#if CONFIG_CAMERA_BOARD_TTGO_TCAM
static esp_err_t display_msg_on_oled(const char *msg) {
	ssd1306_clearScreen();
	int8_t xpos = (int8_t)(ssd1306_displayWidth() - strlen(msg)*10)/2;
	xpos = (xpos < 0)? 0: xpos;
	ssd1306_setFixedFont(ssd1306xled_font5x7);
	ssd1306_printFixedN(xpos, 30, msg, STYLE_BOLD, FONT_SIZE_2X);
	return ESP_OK;
}

static esp_err_t display_pred_on_oled(CustomVisionClient::CustomVisionDetectionResult_t *predResult) {

	ssd1306_clearScreen();
	const CustomVisionClient::CustomVisionDetectionModel_t *bestPred = predResult->getBestPrediction();
	if (bestPred != NULL) {
		ssd1306_setFixedFont(ssd1306xled_font5x7);
		ssd1306_printFixedN(5, 5, "Hello", STYLE_BOLD, FONT_SIZE_2X);
		ssd1306_printFixedN(5, 25, bestPred->tagName, STYLE_BOLD, FONT_SIZE_2X);

		char predProbChar[40];
		sprintf(predProbChar, "Probability: %.2f%s", (bestPred->probability*100), "%");
		ssd1306_setFixedFont(ssd1306xled_font6x8);
		ssd1306_printFixedN(5, 54, predProbChar, STYLE_BOLD, FONT_SIZE_NORMAL);

	} else {
		ssd1306_setFixedFont(ssd1306xled_font5x7);
		ssd1306_printFixedN(5, (ssd1306_displayHeight() - 8)/2, "Nobody there", STYLE_BOLD, FONT_SIZE_2X);
	}

	return ESP_OK;
}
#endif

static esp_err_t recog_handler(httpd_req_t *req){
	esp_err_t res = ESP_OK;

    camera_fb_t *fb = esp_camera_fb_get();

	if (!fb) {
		ESP_LOGE(TAG_HTTPD, "Camera buffer is not accessible");
		httpd_resp_send_500(req);
		res = ESP_FAIL;
	}
	else {
		if (fb->format == PIXFORMAT_JPEG) {
			httpd_resp_set_type(req, "image/jpeg");
			httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

			uint8_t *out_buff = fb->buf;
			size_t out_len = fb->len;

			// To do object detection
			CustomVisionClient *_cvc = (CustomVisionClient*)req->user_ctx;
			if (_cvc != NULL) {

#if CONFIG_CAMERA_BOARD_TTGO_TCAM
				display_msg_on_oled(DETECTING2_MESSAGE);
#endif

				CustomVisionClient::CustomVisionDetectionResult_t *predResult = new CustomVisionClient::CustomVisionDetectionResult_t();

				// Alternative 1 --> use drawInfo true to draw by detect method
//				res = _cvc->detect(fb, predResult, MIN_PROB_BEST_PREDICTION, true, &out_buff, &out_len);
//				if (res != ESP_OK) {
//	//				request->send(500, "text/plain", "Prediction is failed");
//	//				return;
//				}

				// Alternative 2 --> explicitely call putInfoOnFrame to draw bounding box and label
				res = _cvc->detect(fb, predResult, MIN_PROB_BEST_PREDICTION);
				if (res == ESP_OK && predResult->isBestPredictionFound() && !predResult->predictions.empty()) {
					// Draw box and label
					_cvc->putInfoOnFrame(fb, predResult->predictions, MIN_PROB_BEST_PREDICTION*0.75f, &out_buff, &out_len);

#if CONFIG_CAMERA_BOARD_TTGO_TCAM
					// Display label on OLED
					display_pred_on_oled(predResult);
#endif
				}
				else {
					int32_t x = (fb->width - (strlen(DETECT_NO_RESULT_MESSAGE) * 14)) / 2;
					int32_t y = 10;
					_cvc->putLabelOnFrame(fb, DETECT_NO_RESULT_MESSAGE, x, y, FACE_COLOR_RED, &out_buff, &out_len);

#if CONFIG_CAMERA_BOARD_TTGO_TCAM
					display_msg_on_oled("Nobody :(");
#endif
				}

				delete predResult;
				predResult = NULL;
			}

			res = httpd_resp_send(req, (const char *)out_buff, out_len);
		}
	}

	esp_camera_fb_return(fb);

	return res;
}

static esp_err_t stream_handler(httpd_req_t *req){
	camera_fb_t * fb = NULL;
	esp_err_t res = ESP_OK;
	char * part_buf[64];

	size_t _jpg_buf_len = 0;
	uint8_t * _jpg_buf = NULL;

	res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);

	CustomVisionClient *_cvc = (CustomVisionClient*)req->user_ctx;

	int64_t _nextPredictTime = esp_timer_get_time() + STREAM_PREDICTION_INTERVAL/2;
	int64_t _showInfoUntil = 0;

	CustomVisionClient::CustomVisionDetectionResult_t _predRes = {};

	camera_fb_t *_asyncDetectFb = NULL;
	bool _needUpadteOLED = true;

	while(true){

		fb = esp_camera_fb_get();

		if (!fb) {
			ESP_LOGE(TAG_HTTPD, "Camera capture failed");
			res = ESP_FAIL;
			break;
		}

		if (fb->format != PIXFORMAT_JPEG) {
			ESP_LOGE(TAG_HTTPD, "Camera captured images is not JPEG");
			res = ESP_FAIL;
		} else {
//			_jpg_buf_len = fb->len;
//			_jpg_buf = fb->buf;
		}

		if(res == ESP_OK) {

			if (esp_timer_get_time() > _nextPredictTime) {
				if (_cvc != NULL) {

					// Clear prev predictions
					_predRes.clear();

					// Copy camera framebuffer to be sent for detection
					_asyncDetectFb =  (camera_fb_t*)malloc(sizeof(camera_fb_t));
					_asyncDetectFb->len = fb->len;
					_asyncDetectFb->width = fb->width;
					_asyncDetectFb->height = fb->height;
					_asyncDetectFb->format = fb->format;
					_asyncDetectFb->buf =  (uint8_t*)malloc(fb->len);
					memcpy(_asyncDetectFb->buf, fb->buf, fb->len);

					// Do detection asynchronously, so the video stream not interrupted
					res = _cvc->detectAsync(_asyncDetectFb, MIN_PROB_BEST_PREDICTION);

					app_state = APP_START_RECOGNITION;
					_showInfoUntil = esp_timer_get_time() + 3000000;

					_nextPredictTime = esp_timer_get_time() + (STREAM_PREDICTION_INTERVAL);
//					vTaskDelay(50/portTICK_PERIOD_MS);

					ESP_LOGI(TAG_HTTPD, "Free heap before pred: %ul", esp_get_free_heap_size());

					_needUpadteOLED = true;
				}
			}

			// Check queue from aysnc prediction
			if (app_state == APP_START_RECOGNITION && _cvc->detectionQueue) {

				if (xQueueReceive(_cvc->detectionQueue, &_predRes, 5) != pdFALSE) {

					app_state = APP_DONE_RECOGNITION;
					_showInfoUntil = esp_timer_get_time() + STREAM_PREDICTION_INTERVAL/2;

					// Update next prediction time
					_nextPredictTime = esp_timer_get_time() + (STREAM_PREDICTION_INTERVAL);

					// Free prev resource used for detection
					if (_asyncDetectFb != NULL) {
						free(_asyncDetectFb->buf);
						_asyncDetectFb->buf = NULL;
						free(_asyncDetectFb);
						_asyncDetectFb = NULL;
					}

					ESP_LOGI(TAG_HTTPD, "Free heap after pred: %ul", esp_get_free_heap_size());

					_needUpadteOLED = true;
				}
			}

			if (app_state == APP_START_RECOGNITION && (esp_timer_get_time() < _showInfoUntil)) {
				int32_t x = (fb->width - (strlen(DETECTING_MESSAGE) * 14)) / 2;
				int32_t y = 10;
				_cvc->putLabelOnFrame(fb, DETECTING_MESSAGE, x, y, FACE_COLOR_YELLOW, &_jpg_buf, &_jpg_buf_len);

#if CONFIG_CAMERA_BOARD_TTGO_TCAM
				//Since it's in the loop, guard this so only do it when need to update
				if (_needUpadteOLED) {
					display_msg_on_oled(DETECTING2_MESSAGE);
					_needUpadteOLED = false;
				}
#endif
			}

			if (app_state == APP_DONE_RECOGNITION && (esp_timer_get_time() < _showInfoUntil)) {

				if (_predRes.isBestPredictionFound() && !_predRes.predictions.empty()) {
					// If prediction is there, draw it
					_cvc->putInfoOnFrame(fb, _predRes.predictions, MIN_PROB_BEST_PREDICTION*0.75f, &_jpg_buf, &_jpg_buf_len);

#if CONFIG_CAMERA_BOARD_TTGO_TCAM
					// Display label on OLED
					if (_needUpadteOLED) {
						display_pred_on_oled(&_predRes);
						_needUpadteOLED = false;
					}
#endif

				} else {
					int32_t x = (fb->width - (strlen(DETECT_NO_RESULT_MESSAGE) * 14)) / 2;
					int32_t y = 10;
					_cvc->putLabelOnFrame(fb, DETECT_NO_RESULT_MESSAGE, x, y, FACE_COLOR_RED, &_jpg_buf, &_jpg_buf_len);

#if CONFIG_CAMERA_BOARD_TTGO_TCAM
					if (_needUpadteOLED) {
						display_msg_on_oled("Nobody :(");
						_needUpadteOLED = false;
					}
#endif
				}
			}
		}

		// If still NULL, just use camera framebuffer
		if (_jpg_buf == NULL) {
			_jpg_buf_len = fb->len;
			_jpg_buf = fb->buf;
		}

		if (res == ESP_OK) {
			size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
			res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
		}
		if (res == ESP_OK) {
			res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
		}
		if (res == ESP_OK) {
			res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
		}

		if (fb) {

			// This should happen when _jpg_buf is NOT coming from camera's framebuffer. Important to free it.
			if (fb->buf != _jpg_buf) {
				free(_jpg_buf);
			}
			_jpg_buf = NULL;

			esp_camera_fb_return(fb);
			fb = NULL;

		}
		else {
			if (_jpg_buf) {
				free(_jpg_buf);
				_jpg_buf = NULL;
			}
		}

		if (res != ESP_OK) {
			break;
		}
	}

//	if (asyncPredictFb != NULL) {
//		free(asyncPredictFb->buf);
//		free(asyncPredictFb);
//		asyncPredictFb = NULL;
//	}

	// Shit happens!
	if (res != ESP_OK) {
		httpd_resp_send_500(req);
	}

	return res;
}

void startHttpd(CustomVisionClient *cvc) {

	// Start WiFi connection
	app_state = APP_WAIT_FOR_CONNECT;

	DXWiFi *wifi = DXWiFi::GetInstance(WIFI_MODE_STA);
	wifi->ConnectSync(SSID_NAME, SSID_PASS, portMAX_DELAY);

	app_state = APP_WIFI_CONNECTED;

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.stack_size = (4096*2);
//	config.max_open_sockets = 10;
//	config.send_wait_timeout = 30;
//	config.recv_wait_timeout = 30;

	httpd_uri_t index_uri = {
		.uri       = "/",
		.method    = HTTP_GET,
		.handler   = index_handler,
		.user_ctx  = NULL
	};

	httpd_uri_t capture_uri = {
		.uri       = "/capture",
		.method    = HTTP_GET,
		.handler   = capture_handler,
		.user_ctx  = NULL
	};

	httpd_uri_t recog_uri = {
		.uri       = "/recog",
		.method    = HTTP_GET,
		.handler   = recog_handler,
		.user_ctx  = cvc
	};

	httpd_uri_t stream_uri = {
		.uri       = "/stream",
		.method    = HTTP_GET,
		.handler   = stream_handler,
		.user_ctx  = cvc
	};

	ESP_LOGI(TAG_HTTPD, "Starting web server on port: '%d'", config.server_port);
	if (httpd_start(&camera_httpd, &config) == ESP_OK) {
		httpd_register_uri_handler(camera_httpd, &index_uri);
		httpd_register_uri_handler(camera_httpd, &capture_uri);
		httpd_register_uri_handler(camera_httpd, &recog_uri);
		httpd_register_uri_handler(camera_httpd, &stream_uri);
	}
}


#endif /* MAIN_APP_HTTPD_HPP_ */
