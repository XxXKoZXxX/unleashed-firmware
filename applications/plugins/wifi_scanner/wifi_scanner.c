#include <furi.h>
#include <furi_hal_console.h>
#include <furi_hal_gpio.h>
#include <furi_hal_power.h>
#include <furi_hal_uart.h>
#include <gui/canvas_i.h>
#include <gui/gui.h>
#include <input/input.h>
#include <math.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>

#include <u8g2.h>

#include "FlipperZeroWiFiModuleDefines.h"

#define WIFI_APP_DEBUG 0

#if WIFI_APP_DEBUG
#define APP_NAME_TAG "WiFi_Scanner"
#define WIFI_APP_LOG_I(format, ...) FURI_LOG_I(APP_NAME_TAG, format, ##__VA_ARGS__)
#define WIFI_APP_LOG_D(format, ...) FURI_LOG_D(APP_NAME_TAG, format, ##__VA_ARGS__)
#define WIFI_APP_LOG_E(format, ...) FURI_LOG_E(APP_NAME_TAG, format, ##__VA_ARGS__)
#else
#define WIFI_APP_LOG_I(format, ...)
#define WIFI_APP_LOG_D(format, ...)
#define WIFI_APP_LOG_E(format, ...)
#endif // WIFI_APP_DEBUG

#define DISABLE_CONSOLE !WIFI_APP_DEBUG

#define ENABLE_MODULE_POWER 1
#define ENABLE_MODULE_DETECTION 1

#define ANIMATION_TIME 350

typedef enum EChunkArrayData {
    EChunkArrayData_Context = 0,
    EChunkArrayData_SSID,
    EChunkArrayData_EncryptionType,
    EChunkArrayData_RSSI,
    EChunkArrayData_BSSID,
    EChunkArrayData_Channel,
    EChunkArrayData_IsHidden,
    EChunkArrayData_CurrentAPIndex,
    EChunkArrayData_TotalAps,
    EChunkArrayData_ENUM_MAX
} EChunkArrayData;

typedef enum EEventType // app internally defined event types
{ EventTypeKey // flipper input.h type
} EEventType;

typedef struct SPluginEvent {
    EEventType m_type;
    InputEvent m_input;
} SPluginEvent;

typedef struct EAccessPointDesc {
    FuriString* m_accessPointName;
    int16_t m_rssi;
    FuriString* m_secType;
    FuriString* m_bssid;
    unsigned short m_channel;
    bool m_isHidden;
} EAccessPointDesc;

typedef enum EAppContext {
    Undefined,
    WaitingForModule,
    Initializing,
    ScanMode,
    MonitorMode,
    ScanAnimation,
    MonitorAnimation
} EAppContext;

typedef enum EWorkerEventFlags {
    WorkerEventReserved = (1 << 0), // Reserved for StreamBuffer internal event
    WorkerEventStop = (1 << 1),
    WorkerEventRx = (1 << 2),
} EWorkerEventFlags;

typedef struct SWiFiScannerApp {
    FuriMutex* mutex;
    Gui* m_gui;
    FuriThread* m_worker_thread;
    NotificationApp* m_notification;
    FuriStreamBuffer* m_rx_stream;

    bool m_wifiModuleInitialized;
    bool m_wifiModuleAttached;

    EAppContext m_context;

    EAccessPointDesc m_currentAccesspointDescription;

    unsigned short m_totalAccessPoints;
    unsigned short m_currentIndexAccessPoint;

    uint32_t m_prevAnimationTime;
    uint32_t m_animationTime;
    uint8_t m_animtaionCounter;
} SWiFiScannerApp;

/////// INIT STATE ///////
static void wifi_scanner_app_init(SWiFiScannerApp* const app) {
    app->m_context = Undefined;

    app->m_totalAccessPoints = 0;
    app->m_currentIndexAccessPoint = 0;

    app->m_currentAccesspointDescription.m_accessPointName = furi_string_alloc();
    furi_string_set(app->m_currentAccesspointDescription.m_accessPointName, "N/A\n");
    app->m_currentAccesspointDescription.m_channel = 0;
    app->m_currentAccesspointDescription.m_bssid = furi_string_alloc();
    furi_string_set(app->m_currentAccesspointDescription.m_bssid, "N/A\n");
    app->m_currentAccesspointDescription.m_secType = furi_string_alloc();
    furi_string_set(app->m_currentAccesspointDescription.m_secType, "N/A\n");
    app->m_currentAccesspointDescription.m_rssi = 0;
    app->m_currentAccesspointDescription.m_isHidden = false;

    app->m_prevAnimationTime = 0;
    app->m_animationTime = ANIMATION_TIME;
    app->m_animtaionCounter = 0;

    app->m_wifiModuleInitialized = false;

#if ENABLE_MODULE_DETECTION
    app->m_wifiModuleAttached = false;
#else
    app->m_wifiModuleAttached = true;
#endif
}

int16_t dBmtoPercentage(int16_t dBm) {
    const int16_t RSSI_MAX = -50; // define maximum strength of signal in dBm
    const int16_t RSSI_MIN = -100; // define minimum strength of signal in dBm

    int16_t quality;
    if(dBm <= RSSI_MIN) {
        quality = 0;
    } else if(dBm >= RSSI_MAX) {
        quality = 100;
    } else {
        quality = 2 * (dBm + 100);
    }

    return quality;
}

void DrawSignalStrengthBar(Canvas* canvas, int rssi, int x, int y, int width, int height) {
    int16_t percents = dBmtoPercentage(rssi);

    u8g2_DrawHLine(&canvas->fb, x, y, width);
    u8g2_DrawHLine(&canvas->fb, x, y + height, width);

    if(rssi != NA && height > 0) {
        uint8_t barHeight = floor((height / 100.f) * percents);
        canvas_draw_box(canvas, x, y + height - barHeight, width, barHeight);
    }
}

static void wifi_module_render_callback(Canvas* const canvas, void* ctx) {
    furi_assert(ctx);
    SWiFiScannerApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);

    {
        switch(app->m_context) {
        case Undefined: {
            canvas_set_font(canvas, FontPrimary);

            const char* strError = "Something wrong";
            canvas_draw_str(
                canvas,
                (u8g2_GetDisplayWidth(&canvas->fb) / 2) -
                    (canvas_string_width(canvas, strError) / 2),
                (u8g2_GetDisplayHeight(&canvas->fb) /
                 2) /* - (u8g2_GetMaxCharHeight(&canvas->fb) / 2)*/,
                strError);
        } break;
        case WaitingForModule:
#if ENABLE_MODULE_DETECTION
            furi_assert(!app->m_wifiModuleAttached);
            if(!app->m_wifiModuleAttached) {
                canvas_set_font(canvas, FontSecondary);

                const char* strConnectModule = "Attach WiFi scanner module";
                canvas_draw_str(
                    canvas,
                    (u8g2_GetDisplayWidth(&canvas->fb) / 2) -
                        (canvas_string_width(canvas, strConnectModule) / 2),
                    (u8g2_GetDisplayHeight(&canvas->fb) /
                     2) /* - (u8g2_GetMaxCharHeight(&canvas->fb) / 2)*/,
                    strConnectModule);
            }
#endif
            break;
        case Initializing: {
            furi_assert(!app->m_wifiModuleInitialized);
            if(!app->m_wifiModuleInitialized) {
                canvas_set_font(canvas, FontPrimary);

                const char* strInitializing = "Initializing...";
                canvas_draw_str(
                    canvas,
                    (u8g2_GetDisplayWidth(&canvas->fb) / 2) -
                        (canvas_string_width(canvas, strInitializing) / 2),
                    (u8g2_GetDisplayHeight(&canvas->fb) / 2) -
                        (u8g2_GetMaxCharHeight(&canvas->fb) / 2),
                    strInitializing);
            }
        } break;
        case ScanMode: {
            uint8_t offsetY = 0;
            uint8_t offsetX = 0;
            canvas_draw_frame(
                canvas,
                0,
                0,
                u8g2_GetDisplayWidth(&canvas->fb),
                u8g2_GetDisplayHeight(&canvas->fb));

            //canvas_set_font(canvas, FontPrimary);
            u8g2_SetFont(&canvas->fb, u8g2_font_7x13B_tr);
            uint8_t fontHeight = u8g2_GetMaxCharHeight(&canvas->fb);

            offsetX += 5;
            offsetY += fontHeight;
            canvas_draw_str(
                canvas,
                offsetX,
                offsetY,
                app->m_currentAccesspointDescription.m_isHidden ?
                    "(Hidden SSID)" :
                    furi_string_get_cstr(app->m_currentAccesspointDescription.m_accessPointName));

            offsetY += fontHeight;

            canvas_draw_str(
                canvas,
                offsetX,
                offsetY,
                furi_string_get_cstr(app->m_currentAccesspointDescription.m_bssid));

            canvas_set_font(canvas, FontSecondary);
            //u8g2_SetFont(&canvas->fb, u8g2_font_tinytim_tf);
            fontHeight = u8g2_GetMaxCharHeight(&canvas->fb);

            offsetY += fontHeight + 1;

            char string[15];
            snprintf(
                string, sizeof(string), "RSSI: %d", app->m_currentAccesspointDescription.m_rssi);
            canvas_draw_str(canvas, offsetX, offsetY, string);

            offsetY += fontHeight + 1;

            snprintf(
                string, sizeof(string), "CHNL: %d", app->m_currentAccesspointDescription.m_channel);
            canvas_draw_str(canvas, offsetX, offsetY, string);

            offsetY += fontHeight + 1;

            snprintf(
                string,
                sizeof(string),
                "ENCR: %s",
                furi_string_get_cstr(app->m_currentAccesspointDescription.m_secType));
            canvas_draw_str(canvas, offsetX, offsetY, string);

            offsetY += fontHeight;
            offsetY -= fontHeight;

            u8g2_SetFont(&canvas->fb, u8g2_font_courB08_tn);
            snprintf(
                string,
                sizeof(string),
                "%d/%d",
                app->m_currentIndexAccessPoint,
                app->m_totalAccessPoints);
            offsetX = u8g2_GetDisplayWidth(&canvas->fb) - canvas_string_width(canvas, string) - 5;
            canvas_draw_str(canvas, offsetX, offsetY, string);

            canvas_draw_frame(
                canvas,
                offsetX - 6,
                offsetY - u8g2_GetMaxCharHeight(&canvas->fb) - 3,
                u8g2_GetDisplayWidth(&canvas->fb),
                u8g2_GetDisplayHeight(&canvas->fb));

            u8g2_SetFont(&canvas->fb, u8g2_font_open_iconic_arrow_2x_t);
            if(app->m_currentIndexAccessPoint != app->m_totalAccessPoints) {
                //canvas_draw_triangle(canvas, offsetX - 5 - 20, offsetY + 5, 4, 4, CanvasDirectionBottomToTop);
                canvas_draw_str(canvas, offsetX - 0 - 35, offsetY + 5, "\x4C");
            }

            if(app->m_currentIndexAccessPoint != 1) {
                //canvas_draw_triangle(canvas, offsetX - 6 - 35, offsetY + 5, 4, 4, CanvasDirectionTopToBottom);
                canvas_draw_str(canvas, offsetX - 4 - 20, offsetY + 5, "\x4F");
            }
        } break;
        case MonitorMode: {
            uint8_t offsetY = 0;
            uint8_t offsetX = 0;

            canvas_draw_frame(
                canvas,
                0,
                0,
                u8g2_GetDisplayWidth(&canvas->fb),
                u8g2_GetDisplayHeight(&canvas->fb));

            //canvas_set_font(canvas, FontBigNumbers);
            u8g2_SetFont(&canvas->fb, u8g2_font_inb27_mr);
            uint8_t fontHeight = u8g2_GetMaxCharHeight(&canvas->fb);
            uint8_t fontWidth = u8g2_GetMaxCharWidth(&canvas->fb);

            if(app->m_currentAccesspointDescription.m_rssi == NA) {
                offsetX += floor(u8g2_GetDisplayWidth(&canvas->fb) / 2) - fontWidth - 10;
                offsetY += fontHeight - 5;

                canvas_draw_str(canvas, offsetX, offsetY, "N/A");
            } else {
                offsetX += floor(u8g2_GetDisplayWidth(&canvas->fb) / 2) - 2 * fontWidth;
                offsetY += fontHeight - 5;

                char rssi[8];
                snprintf(rssi, sizeof(rssi), "%d", app->m_currentAccesspointDescription.m_rssi);
                canvas_draw_str(canvas, offsetX, offsetY, rssi);
            }

            //canvas_set_font(canvas, FontPrimary);
            u8g2_SetFont(&canvas->fb, u8g2_font_7x13B_tr);
            fontHeight = u8g2_GetMaxCharHeight(&canvas->fb);
            fontWidth = u8g2_GetMaxCharWidth(&canvas->fb);

            offsetX = 5;
            offsetY = u8g2_GetDisplayHeight(&canvas->fb) - 7 - fontHeight;
            canvas_draw_str(
                canvas,
                offsetX,
                offsetY,
                furi_string_get_cstr(app->m_currentAccesspointDescription.m_accessPointName));

            offsetY += fontHeight + 2;

            canvas_draw_str(
                canvas,
                offsetX,
                offsetY,
                furi_string_get_cstr(app->m_currentAccesspointDescription.m_bssid));

            DrawSignalStrengthBar(
                canvas, app->m_currentAccesspointDescription.m_rssi, 5, 5, 12, 25);
            DrawSignalStrengthBar(
                canvas,
                app->m_currentAccesspointDescription.m_rssi,
                u8g2_GetDisplayWidth(&canvas->fb) - 5 - 12,
                5,
                12,
                25);
        } break;
        case ScanAnimation: {
            uint32_t currentTime = furi_get_tick();
            if(currentTime - app->m_prevAnimationTime > app->m_animationTime) {
                app->m_prevAnimationTime = currentTime;
                app->m_animtaionCounter += 1;
                app->m_animtaionCounter = app->m_animtaionCounter % 3;
            }

            uint8_t offsetX = 10;
            uint8_t mutliplier = 2;

            for(uint8_t i = 0; i < 3; ++i) {
                canvas_draw_disc(
                    canvas,
                    offsetX + 30 + 25 * i,
                    u8g2_GetDisplayHeight(&canvas->fb) / 2 - 7,
                    5 * (app->m_animtaionCounter == i ? mutliplier : 1));
            }

            u8g2_SetFont(&canvas->fb, u8g2_font_7x13B_tr);
            //canvas_set_font(canvas, FontPrimary);
            const char* message = "Scanning";
            canvas_draw_str(
                canvas,
                u8g2_GetDisplayWidth(&canvas->fb) / 2 - canvas_string_width(canvas, message) / 2,
                55,
                message);
        } break;
        case MonitorAnimation: {
            uint32_t currentTime = furi_get_tick();
            if(currentTime - app->m_prevAnimationTime > app->m_animationTime) {
                app->m_prevAnimationTime = currentTime;
                app->m_animtaionCounter += 1;
                app->m_animtaionCounter = app->m_animtaionCounter % 2;
            }

            uint8_t offsetX = 10;
            uint8_t mutliplier = 2;

            canvas_draw_disc(
                canvas,
                offsetX + 30,
                u8g2_GetDisplayHeight(&canvas->fb) / 2 - 7,
                5 * (app->m_animtaionCounter == 0 ? mutliplier : 1));
            canvas_draw_disc(
                canvas,
                offsetX + 55,
                u8g2_GetDisplayHeight(&canvas->fb) / 2 - 7,
                5 * (app->m_animtaionCounter == 1 ? mutliplier : 1));
            canvas_draw_disc(
                canvas,
                offsetX + 80,
                u8g2_GetDisplayHeight(&canvas->fb) / 2 - 7,
                5 * (app->m_animtaionCounter == 0 ? mutliplier : 1));

            u8g2_SetFont(&canvas->fb, u8g2_font_7x13B_tr);
            //canvas_set_font(canvas, FontPrimary);
            const char* message = "Monitor Mode";
            canvas_draw_str(
                canvas,
                u8g2_GetDisplayWidth(&canvas->fb) / 2 - canvas_string_width(canvas, message) / 2,
                55,
                message);
        } break;
        default:
            break;
        }
    }
    furi_mutex_release(app->mutex);
}

static void wifi_module_input_callback(InputEvent* input_event, FuriMessageQueue* event_queue) {
    furi_assert(event_queue);

    SPluginEvent event = {.m_type = EventTypeKey, .m_input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

static void uart_on_irq_cb(UartIrqEvent ev, uint8_t data, void* context) {
    furi_assert(context);

    SWiFiScannerApp* app = context;

    WIFI_APP_LOG_I("uart_echo_on_irq_cb");

    if(ev == UartIrqEventRXNE) {
        WIFI_APP_LOG_I("ev == UartIrqEventRXNE");
        furi_stream_buffer_send(app->m_rx_stream, &data, 1, 0);
        furi_thread_flags_set(furi_thread_get_id(app->m_worker_thread), WorkerEventRx);
    }
}

static int32_t uart_worker(void* context) {
    furi_assert(context);

    SWiFiScannerApp* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(app == NULL) {
        return 1;
    }

    FuriStreamBuffer* rx_stream = app->m_rx_stream;

    furi_mutex_release(app->mutex);

    while(true) {
        uint32_t events = furi_thread_flags_wait(
            WorkerEventStop | WorkerEventRx, FuriFlagWaitAny, FuriWaitForever);
        furi_check((events & FuriFlagError) == 0);

        if(events & WorkerEventStop) break;
        if(events & WorkerEventRx) {
            size_t length = 0;
            FuriString* receivedString;
            receivedString = furi_string_alloc();
            do {
                uint8_t data[64];
                length = furi_stream_buffer_receive(rx_stream, data, 64, 25);
                if(length > 0) {
                    WIFI_APP_LOG_I("Received Data - length: %i", length);

                    for(uint16_t i = 0; i < length; i++) {
                        furi_string_push_back(receivedString, data[i]);
                    }

                    //notification_message(app->notification, &sequence_set_only_red_255);
                }
            } while(length > 0);
            if(furi_string_size(receivedString) > 0) {
                FuriString* chunk;
                chunk = furi_string_alloc();
                size_t begin = 0;
                size_t end = 0;
                size_t stringSize = furi_string_size(receivedString);

                WIFI_APP_LOG_I("Received string: %s", furi_string_get_cstr(receivedString));

                FuriString* chunksArray[EChunkArrayData_ENUM_MAX];
                for(uint8_t i = 0; i < EChunkArrayData_ENUM_MAX; ++i) {
                    chunksArray[i] = furi_string_alloc();
                }

                uint8_t index = 0;
                do {
                    end = furi_string_search_char(receivedString, '+', begin);

                    if(end == FURI_STRING_FAILURE) {
                        end = stringSize;
                    }

                    WIFI_APP_LOG_I("size: %i, begin: %i, end: %i", stringSize, begin, end);

                    furi_string_set_strn(
                        chunk, &furi_string_get_cstr(receivedString)[begin], end - begin);

                    WIFI_APP_LOG_I("String chunk: %s", furi_string_get_cstr(chunk));

                    furi_string_set(chunksArray[index++], chunk);

                    begin = end + 1;
                } while(end < stringSize);
                furi_string_free(chunk);

                app = context;
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                if(app == NULL) {
                    return 1;
                }

                if(!app->m_wifiModuleInitialized) {
                    if(furi_string_cmp_str(
                           chunksArray[EChunkArrayData_Context], MODULE_CONTEXT_INITIALIZATION) ==
                       0) {
                        app->m_wifiModuleInitialized = true;
                        app->m_context = ScanAnimation;
                    }

                } else {
                    if(furi_string_cmp_str(
                           chunksArray[EChunkArrayData_Context], MODULE_CONTEXT_MONITOR) == 0) {
                        app->m_context = MonitorMode;
                    } else if(
                        furi_string_cmp_str(
                            chunksArray[EChunkArrayData_Context], MODULE_CONTEXT_SCAN) == 0) {
                        app->m_context = ScanMode;
                    } else if(
                        furi_string_cmp_str(
                            chunksArray[EChunkArrayData_Context], MODULE_CONTEXT_SCAN_ANIMATION) ==
                        0) {
                        app->m_context = ScanAnimation;
                    } else if(
                        furi_string_cmp_str(
                            chunksArray[EChunkArrayData_Context],
                            MODULE_CONTEXT_MONITOR_ANIMATION) == 0) {
                        app->m_context = MonitorAnimation;
                    }

                    if(app->m_context == MonitorMode || app->m_context == ScanMode) {
                        furi_string_set(
                            app->m_currentAccesspointDescription.m_accessPointName,
                            chunksArray[EChunkArrayData_SSID]);
                        furi_string_set(
                            app->m_currentAccesspointDescription.m_secType,
                            chunksArray[EChunkArrayData_EncryptionType]);
                        app->m_currentAccesspointDescription.m_rssi =
                            atoi(furi_string_get_cstr(chunksArray[EChunkArrayData_RSSI]));
                        furi_string_set(
                            app->m_currentAccesspointDescription.m_bssid,
                            chunksArray[EChunkArrayData_BSSID]);
                        app->m_currentAccesspointDescription.m_channel =
                            atoi(furi_string_get_cstr(chunksArray[EChunkArrayData_Channel]));
                        app->m_currentAccesspointDescription.m_isHidden =
                            atoi(furi_string_get_cstr(chunksArray[EChunkArrayData_IsHidden]));

                        app->m_currentIndexAccessPoint = atoi(
                            furi_string_get_cstr(chunksArray[EChunkArrayData_CurrentAPIndex]));
                        app->m_totalAccessPoints =
                            atoi(furi_string_get_cstr(chunksArray[EChunkArrayData_TotalAps]));
                    }
                }

                furi_mutex_release(app->mutex);

                // Clear string array
                for(index = 0; index < EChunkArrayData_ENUM_MAX; ++index) {
                    furi_string_free(chunksArray[index]);
                }
            }
            furi_string_free(receivedString);
        }
    }

    return 0;
}

typedef enum ESerialCommand {
    ESerialCommand_Next,
    ESerialCommand_Previous,
    ESerialCommand_Scan,
    ESerialCommand_MonitorMode,
    ESerialCommand_Restart
} ESerialCommand;

void send_serial_command(ESerialCommand command) {
#if !DISABLE_CONSOLE
    return;
#endif

    uint8_t data[1] = {0};

    switch(command) {
    case ESerialCommand_Next:
        data[0] = MODULE_CONTROL_COMMAND_NEXT;
        break;
    case ESerialCommand_Previous:
        data[0] = MODULE_CONTROL_COMMAND_PREVIOUS;
        break;
    case ESerialCommand_Scan:
        data[0] = MODULE_CONTROL_COMMAND_SCAN;
        break;
    case ESerialCommand_MonitorMode:
        data[0] = MODULE_CONTROL_COMMAND_MONITOR;
        break;
    case ESerialCommand_Restart:
        data[0] = MODULE_CONTROL_COMMAND_RESTART;
        break;
    default:
        return;
    };

    furi_hal_uart_tx(FuriHalUartIdUSART1, data, 1);
}

int32_t wifi_scanner_app(void* p) {
    UNUSED(p);

    WIFI_APP_LOG_I("Init");

    // FuriTimer* timer = furi_timer_alloc(blink_test_update, FuriTimerTypePeriodic, event_queue);
    // furi_timer_start(timer, furi_kernel_get_tick_frequency());

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(SPluginEvent));

    SWiFiScannerApp* app = malloc(sizeof(SWiFiScannerApp));

    wifi_scanner_app_init(app);

#if ENABLE_MODULE_DETECTION
    furi_hal_gpio_init(
        &gpio_ext_pc0,
        GpioModeInput,
        GpioPullUp,
        GpioSpeedLow); // Connect to the Flipper's ground just to be sure
    //furi_hal_gpio_add_int_callback(pinD0, input_isr_d0, this);
    app->m_context = WaitingForModule;
#else
    app->m_context = Initializing;
#if ENABLE_MODULE_POWER
    furi_hal_power_enable_otg();
#endif // ENABLE_MODULE_POWER
#endif // ENABLE_MODULE_DETECTION

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    if(!app->mutex) {
        WIFI_APP_LOG_E("cannot create mutex\r\n");
        free(app);
        return 255;
    }

    WIFI_APP_LOG_I("Mutex created");

    app->m_notification = furi_record_open(RECORD_NOTIFICATION);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, wifi_module_render_callback, app);
    view_port_input_callback_set(view_port, wifi_module_input_callback, event_queue);

    // Open GUI and register view_port
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    //notification_message(app->notification, &sequence_set_only_blue_255);

    app->m_rx_stream = furi_stream_buffer_alloc(1 * 1024, 1);

    app->m_worker_thread = furi_thread_alloc();
    furi_thread_set_name(app->m_worker_thread, "WiFiModuleUARTWorker");
    furi_thread_set_stack_size(app->m_worker_thread, 1024);
    furi_thread_set_context(app->m_worker_thread, app);
    furi_thread_set_callback(app->m_worker_thread, uart_worker);
    furi_thread_start(app->m_worker_thread);
    WIFI_APP_LOG_I("UART thread allocated");

    // Enable uart listener
#if DISABLE_CONSOLE
    furi_hal_console_disable();
#endif
    furi_hal_uart_set_br(FuriHalUartIdUSART1, FLIPPERZERO_SERIAL_BAUD);
    furi_hal_uart_set_irq_cb(FuriHalUartIdUSART1, uart_on_irq_cb, app);
    WIFI_APP_LOG_I("UART Listener created");

    // Because we assume that module was on before we launched the app. We need to ensure that module will be in initial state on app start
    send_serial_command(ESerialCommand_Restart);

    SPluginEvent event;
    for(bool processing = true; processing;) {
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, 100);
        furi_mutex_acquire(app->mutex, FuriWaitForever);

#if ENABLE_MODULE_DETECTION
        if(!app->m_wifiModuleAttached) {
            if(furi_hal_gpio_read(&gpio_ext_pc0) == false) {
                WIFI_APP_LOG_I("Module Attached");
                app->m_wifiModuleAttached = true;
                app->m_context = Initializing;
#if ENABLE_MODULE_POWER
                furi_hal_power_enable_otg();
#endif
            }
        }
#endif // ENABLE_MODULE_DETECTION

        if(event_status == FuriStatusOk) {
            if(event.m_type == EventTypeKey) {
                if(app->m_wifiModuleInitialized) {
                    if(app->m_context == ScanMode) {
                        switch(event.m_input.key) {
                        case InputKeyUp:
                        case InputKeyLeft:
                            if(event.m_input.type == InputTypeShort) {
                                WIFI_APP_LOG_I("Previous");
                                send_serial_command(ESerialCommand_Previous);
                            } else if(event.m_input.type == InputTypeRepeat) {
                                WIFI_APP_LOG_I("Previous Repeat");
                                send_serial_command(ESerialCommand_Previous);
                            }
                            break;
                        case InputKeyDown:
                        case InputKeyRight:
                            if(event.m_input.type == InputTypeShort) {
                                WIFI_APP_LOG_I("Next");
                                send_serial_command(ESerialCommand_Next);
                            } else if(event.m_input.type == InputTypeRepeat) {
                                WIFI_APP_LOG_I("Next Repeat");
                                send_serial_command(ESerialCommand_Next);
                            }
                            break;
                        default:
                            break;
                        }
                    }

                    switch(event.m_input.key) {
                    case InputKeyOk:
                        if(event.m_input.type == InputTypeShort) {
                            if(app->m_context == ScanMode) {
                                WIFI_APP_LOG_I("Monitor Mode");
                                send_serial_command(ESerialCommand_MonitorMode);
                            }
                        } else if(event.m_input.type == InputTypeLong) {
                            WIFI_APP_LOG_I("Scan");
                            send_serial_command(ESerialCommand_Scan);
                        }
                        break;
                    case InputKeyBack:
                        if(event.m_input.type == InputTypeShort) {
                            switch(app->m_context) {
                            case MonitorMode:
                                send_serial_command(ESerialCommand_Scan);
                                break;
                            case ScanMode:
                                processing = false;
                                break;
                            default:
                                break;
                            }
                        } else if(event.m_input.type == InputTypeLong) {
                            processing = false;
                        }
                        break;
                    default:
                        break;
                    }
                } else {
                    if(event.m_input.key == InputKeyBack) {
                        if(event.m_input.type == InputTypeShort ||
                           event.m_input.type == InputTypeLong) {
                            processing = false;
                        }
                    }
                }
            }
        }

#if ENABLE_MODULE_DETECTION
        if(app->m_wifiModuleAttached && furi_hal_gpio_read(&gpio_ext_pc0) == true) {
            WIFI_APP_LOG_D("Module Disconnected - Exit");
            processing = false;
            app->m_wifiModuleAttached = false;
            app->m_wifiModuleInitialized = false;
        }
#endif

        view_port_update(view_port);
        furi_mutex_release(app->mutex);
    }

    WIFI_APP_LOG_I("Start exit app");

    furi_thread_flags_set(furi_thread_get_id(app->m_worker_thread), WorkerEventStop);
    furi_thread_join(app->m_worker_thread);
    furi_thread_free(app->m_worker_thread);

    WIFI_APP_LOG_I("Thread Deleted");

    // Reset GPIO pins to default state
    furi_hal_gpio_init(&gpio_ext_pc0, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

#if DISABLE_CONSOLE
    furi_hal_console_enable();
#endif

    view_port_enabled_set(view_port, false);

    gui_remove_view_port(gui, view_port);

    // Close gui record
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    app->m_gui = NULL;

    view_port_free(view_port);

    furi_message_queue_free(event_queue);

    furi_stream_buffer_free(app->m_rx_stream);

    furi_mutex_free(app->mutex);

    // Free rest
    free(app);

    WIFI_APP_LOG_I("App freed");

#if ENABLE_MODULE_POWER
    furi_hal_power_disable_otg();
#endif

    return 0;
}