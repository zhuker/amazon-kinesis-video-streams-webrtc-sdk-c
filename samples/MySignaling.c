#include "MySignaling.h"
#include <termios.h>

// https://stackoverflow.com/questions/39546500/how-to-make-scanf-to-read-more-than-4095-characters-given-as-input
int clear_icanon(void)
{
    struct termios settings = {0};
    if (tcgetattr(STDIN_FILENO, &settings) < 0) {
        perror("error in tcgetattr");
        return 0;
    }

    settings.c_lflag &= ~ICANON;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) < 0) {
        perror("error in tcsetattr");
        return 0;
    }
    return 1;
}

STATUS copyPasteSignalingConnect(struct SimpleSignaling* self)
{
    clear_icanon(); // Changes the input mode of terminal from canonical mode to non canonical mode to allow copy-paste of over 4096 bytes
    // equivalent to running "stty -icanon"
    printf("---- Please paste in the message here from the other peer ----\n");
    return STATUS_SUCCESS;
}

STATUS copyPasteSignalingReceiveOffer(struct SimpleSignaling* self, PRtcSessionDescriptionInit pOffer)
{
    STATUS retStatus = 0;

    char offer[8192] = {0};
    fgets(offer, 8192, stdin);
    CHK_STATUS(deserializeSessionDescriptionInit(offer, STRLEN(offer), pOffer));
    pOffer->type = SDP_TYPE_OFFER;
CleanUp:
    return retStatus;
}

STATUS copyPasteSignalingSendAnswer(struct SimpleSignaling* self, PRtcSessionDescriptionInit pAnswer)
{
    STATUS retStatus = 0;
    char json[8192] = {0};
    UINT32 sz = 8192;
    CHK_STATUS(serializeSessionDescriptionInit(pAnswer, json, &sz));
    printf("---- Please copy and send this message to the other peer ----\n");
    printf("%s\n", json);

CleanUp:
    return retStatus;
}

STATUS signalingDestroy(struct SimpleSignaling* self)
{
    return STATUS_SUCCESS;
}

STATUS copyPasteSignalingInit(struct CopyPasteSignaling* self)
{
    self->iface.connect = copyPasteSignalingConnect;
    self->iface.receiveOffer = copyPasteSignalingReceiveOffer;
    self->iface.sendAnswer = copyPasteSignalingSendAnswer;
    self->iface.destroy = signalingDestroy;
}

void on_text(struct WebsocketSignaling* self, PCHAR text)
{
    printf("< '%s'\n", text);
    if (STRSTR(text, "type") && STRSTR(text, "offer")) {
        self->offerReceived = TRUE;
    }
}

static int send_text(struct WebsocketSignaling* signaling, struct lws* wsi, PCHAR text)
{
    printf("> '%s'\n", text);
    UINT32 len = STRLEN(text);
    PCHAR buf = signaling->lwsMsgBuf + LWS_PRE;
    STRNCPY(buf, text, len);
    int size = lws_write(wsi, buf, len, LWS_WRITE_TEXT);
    if (size < len) {
        DLOGW("Failed to write out the body of POST request entirely.");
    }
    return size;
}

static int callback_simple_signaling(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len)
{
    struct WebsocketSignaling* signaling = lws_context_user(lws_get_context(wsi));
    switch (reason) {
        /* because we are protocols[0] ... */
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            lwsl_err("CLIENT_CONNECTION_ERROR: %s\n", in ? (char*) in : "(null)");
            signaling->client_wsi = NULL;
            break;

        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            lwsl_user("%s: established\n", __func__);
            lws_callback_on_writable(wsi);
            break;
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
            DLOGD("Client append handshake header\n");
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            lwsl_user("%s: LWS_CALLBACK_CLIENT_WRITEABLE\n", __func__);

            if (!signaling->helloSent) {
                signaling->helloSent = TRUE;
                send_text(signaling, wsi, "HELLO 4243");
            }
            if (signaling->helloReceived && !signaling->sessionRequested) {
                signaling->sessionRequested = TRUE;
                send_text(signaling, wsi, "SESSION 4242");
            }
            if (signaling->sessionOk && !signaling->offerRequested) {
                signaling->offerRequested = TRUE;
                send_text(signaling, wsi, "OFFER_REQUEST");
            }
            BOOL empty = TRUE;
            stackQueueIsEmpty(&signaling->pendingMessages, &empty);
            if (!empty) {
                UINT64 data = 0;
                stackQueueDequeue(&signaling->pendingMessages, &data);
                PCHAR text = (PCHAR) data;
                send_text(signaling, wsi, text);
                lws_callback_on_writable(wsi);
            }

            break;
        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (lws_frame_is_binary(wsi)) {
                break;
            }

            if (lws_is_first_fragment(wsi)) {
                lwsl_user("RX: [\n");
            }
            lwsl_user("RX: %zu %s\n", len, (const char*) in);
            STRNCPY(signaling->receiveBuffer + signaling->bytesReceived, in, len);
            signaling->bytesReceived += len;
            signaling->bytesAvailable = signaling->bytesReceived;
            signaling->receiveBuffer[signaling->bytesReceived] = '\0';
            if (!STRNCMP("HELLO", in, 5)) {
                signaling->helloReceived = TRUE;
            } else if (!STRNCMP("SESSION_OK", in, strlen("SESSION_OK"))) {
                signaling->sessionOk = TRUE;
            }
            if (lws_is_final_fragment(wsi)) {
                signaling->bytesReceived = 0;
                on_text(signaling, signaling->receiveBuffer);
                lwsl_user("RX: ]\n");
                lws_callback_on_writable(wsi);
            }
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            lwsl_user("%s: LWS_CALLBACK_CLIENT_CLOSED\n", __func__);
            signaling->client_wsi = NULL;
            break;

        default:
            break;
    }

    return lws_callback_http_dummy(wsi, reason, user, in, len);
}

STATUS websocketSignalingConnect(struct SimpleSignaling* _self)
{
    struct WebsocketSignaling* self = (struct WebsocketSignaling*) _self;
    struct lws_context_creation_info ctxi = {0};
    struct lws_client_connect_info conni = {0};
    int n = 0, logs = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;

    lws_set_log_level(logs, NULL);

    ctxi.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    ctxi.port = CONTEXT_PORT_NO_LISTEN; /* we do not run any server */
    ctxi.protocols = self->protocols;
    ctxi.fd_limit_per_thread = 1 + 1 + 1;
    ctxi.user = self;

    self->context = lws_create_context(&ctxi);
    if (!self->context) {
        lwsl_err("lws init failed\n");
        return 1;
    }

    conni.context = self->context;
    conni.port = 8443;
    conni.address = "zhuker.video";
    conni.path = "";
    conni.host = conni.address;
    conni.origin = conni.address;
    conni.ssl_connection = LCCSCF_USE_SSL;
    conni.protocol = self->protocols[0].name;
    conni.pwsi = &self->client_wsi;

    lws_client_connect_via_info(&conni);

    return STATUS_SUCCESS;
}

STATUS websocketSignalingReceiveOffer(struct SimpleSignaling* _self, PRtcSessionDescriptionInit pOffer)
{
    STATUS retStatus = 0;
    struct WebsocketSignaling* self = (struct WebsocketSignaling*) _self;
    int n = 0;
    while (n >= 0 && self->client_wsi && !self->offerReceived && !self->iface.interrupted)
        n = lws_service(self->context, 0);
    CHK(self->offerReceived, STATUS_SIGNALING_INVALID_MESSAGE_TYPE);
    if (self->offerReceived) {
        CHK_STATUS(deserializeSessionDescriptionInit(self->receiveBuffer, self->bytesAvailable, pOffer));
        pOffer->type = SDP_TYPE_OFFER;
    }
CleanUp:
    return retStatus;
}

STATUS websocketSignalingSendAnswer(struct SimpleSignaling* _self, PRtcSessionDescriptionInit pAnswer)
{
    struct WebsocketSignaling* self = (struct WebsocketSignaling*) _self;
    STATUS retStatus = 0;
    char json[8192] = {0};
    UINT32 sz = 8192;
    CHK_STATUS(serializeSessionDescriptionInit(pAnswer, json, &sz));
    printf("answer: %s\n", json);
    CHK_STATUS(stackQueueEnqueue(&self->pendingMessages, json));
    int n = 0;
    BOOL answerSent = FALSE;
    while (n >= 0 && self->client_wsi && !answerSent && !self->iface.interrupted) {
        n = lws_service(self->context, 0);
        CHK_STATUS(stackQueueIsEmpty(&self->pendingMessages, &answerSent));
    }

CleanUp:
    return retStatus;
}

STATUS websocketSignalingDestroy(struct SimpleSignaling* _self)
{
    struct WebsocketSignaling* self = (struct WebsocketSignaling*) _self;
    lws_context_destroy(self->context);
    return STATUS_SUCCESS;
}

void websocketSignalingInit(struct WebsocketSignaling* self)
{
    self->iface.connect = websocketSignalingConnect;
    self->iface.receiveOffer = websocketSignalingReceiveOffer;
    self->iface.sendAnswer = websocketSignalingSendAnswer;
    self->iface.destroy = websocketSignalingDestroy;
    self->protocols[0].name = "simple-signaling";
    self->protocols[0].callback = callback_simple_signaling;
    self->protocols[0].user = self;
}
