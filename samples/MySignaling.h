#include "Samples.h"
#include <libwebsockets.h>

struct SimpleSignaling {
    BOOL interrupted;
    STATUS (*connect)(struct SimpleSignaling* self);
    STATUS (*receiveOffer)(struct SimpleSignaling* self, PRtcSessionDescriptionInit pOffer);
    STATUS (*sendAnswer)(struct SimpleSignaling* self, PRtcSessionDescriptionInit pAnswer);
    STATUS (*destroy)(struct SimpleSignaling* self);
};

struct CopyPasteSignaling {
    struct SimpleSignaling iface;
};

STATUS copyPasteSignalingInit(struct CopyPasteSignaling* self);

struct WebsocketSignaling {
    struct SimpleSignaling iface;
    CHAR ourId[16];
    CHAR remoteId[16];
    struct lws_protocols protocols[2];
    struct lws_context* context;
    struct lws* client_wsi;
    CHAR lwsMsgBuf[16384];
    BOOL helloSent;
    BOOL helloReceived;
    BOOL sessionRequested;
    BOOL sessionOk;
    BOOL offerRequested;
    CHAR receiveBuffer[16384];
    UINT32 bytesReceived;
    UINT32 bytesAvailable;
    StackQueue pendingMessages;
    BOOL offerReceived;
    BOOL sessionError;
};

void websocketSignalingInit(struct WebsocketSignaling* self);
