/*
    websockets.c - Test WebSockets
 */
#include "esp.h"

/*
    Diagnostic trace for tests
 */
static void traceEvent(HttpConn *conn, int event, int arg)
{
    HttpPacket  *packet;

    if (event == HTTP_EVENT_READABLE) {
        /*
            Peek at the readq rather than doing httpGetPacket()
            The last frame in a message has packet->last == true
         */
        packet = conn->readq->first;
        mprLog(3, "websock.c: read %s event, last %d", packet->type == WS_MSG_TEXT ? "text" : "binary", packet->last);
        mprLog(3, "websock.c: read: (start of data only) \"%s\"", snclone(mprGetBufStart(packet->content), 40));

    } else if (event == HTTP_EVENT_APP_CLOSE) {
        mprLog(3, "websock.c: close event. Status status %d, orderly closed %d, reason %s", arg,
            httpWebSocketOrderlyClosed(conn), httpGetWebSocketCloseReason(conn));

    } else if (event == HTTP_EVENT_ERROR) {
        mprLog(2, "websock.c: error event");
    }
}

static void dummy_callback(HttpConn *conn, int event, int arg)
{
}

static void dummy_action() { 
    dontAutoFinalize();
    httpSetConnNotifier(getConn(), dummy_callback);
}

static void len_callback(HttpConn *conn, int event, int arg)
{
    HttpPacket      *packet;
    HttpWebSocket   *ws;

    traceEvent(conn, event, arg);
    if (event == HTTP_EVENT_READABLE) {
        /*
            Get and discard the packet. traceEvent will have traced it for us.
         */
        packet = httpGetPacket(conn->readq);
        assert(packet);
        /* 
            Ignore precedding packets and just respond and echo the last 
         */
        if (packet->last) {
            ws = conn->rx->webSocket;
            httpSend(conn, "{type: %d, last: %d, length: %d, data: \"%s\"}\n", packet->type, packet->last,
                ws->messageLength, snclone(mprGetBufStart(packet->content), 10));
        }
    }
}

static void len_action() { 
    dontAutoFinalize();
    httpSetConnNotifier(getConn(), len_callback);
}


/*
    Autobahn test echo server
 */
static void echo_callback(HttpConn *conn, int event, int arg)
{
    HttpPacket  *packet;
    MprBuf      *buf;

    if (event == HTTP_EVENT_READABLE) {
        packet = httpGetPacket(conn->readq);

        buf = conn->rx->webSocket->data;
        if (packet->type == WS_MSG_TEXT || packet->type == WS_MSG_BINARY) {
            mprPutBlockToBuf(buf, mprGetBufStart(packet->content), mprGetBufLength(packet->content));
        }
        if (packet->last) {
            mprAddNullToBuf(buf);
            mprTrace(5, "Echo %d bytes: %s", mprGetBufLength(buf), mprGetBufStart(buf));
            httpSendBlock(conn, packet->type, mprGetBufStart(buf), mprGetBufLength(buf), 0);
            mprFlushBuf(buf);
        }
    }
}

static void echo_action() { 
    HttpConn    *conn;

    conn = getConn();
    dontAutoFinalize();
    //  MOB - API
    conn->rx->webSocket->data = mprCreateBuf(0, 0);
    httpSetConnNotifier(conn, echo_callback);
}


/*
    Test sending an empty text message, followed by an orderly close
 */
static void empty_response() 
{
    httpSendBlock(getConn(), WS_MSG_TEXT, "", 0, 0);
    httpSendClose(getConn(), WS_STATUS_OK, "OK");
}


/*
    Big single message written with one send(). The WebSockets filter will break this into frames as required.
 */
static void big_response() 
{
    HttpConn    *conn;
    MprBuf      *buf;
    int         i, count;

    conn = getConn();
    count = 10000;

    /*
        First message is big, but in a single send. The middleware should break this into frames unless you call:
            httpSetWebSocketPreserveFrames(conn, 1);
        This will regard each call to httpSendBlock as a frame.
     */
    buf = mprCreateBuf(0, 0);
    for (i = 0; i < count; i++) {
        mprPutToBuf(buf, "%8d:01234567890123456789012345678901234567890\n", i);
    }
    mprAddNullToBuf(buf);

    if (httpSendBlock(conn, WS_MSG_TEXT, mprGetBufStart(buf), mprGetBufLength(buf), 0) < 0) {
        httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot send big message");
        return;
    }
    httpSendClose(conn, WS_STATUS_OK, "OK");
}

/*
    Multiple-frame response message with explicit continuations.
    The WebSockets filter will encode each call to httpSendBlock into a frame. 
    Even if large blocks are written, HTTP_MORE assures that the block will be encoded as a single frame.
 */
static void frames_response() 
{
    HttpConn    *conn;
    cchar       *str;
    int         i, more, count;

    conn = getConn();
    count = 1000;

    for (i = 0; i < count; i++) {
        str = sfmt("%8d: Hello\n", i);
        more = (i < (count - 1)) ? HTTP_MORE : 0;
        if (httpSendBlock(conn, WS_MSG_TEXT, str, slen(str), HTTP_BUFFER | more) < 0) {
            httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot send message: %d", i);
            return;
        }
    }
    httpSendClose(conn, WS_STATUS_OK, "OK");
}


ESP_EXPORT int esp_module_websockets(HttpRoute *route, MprModule *module) {
    espDefineAction(route, "basic-construct", dummy_action);
    espDefineAction(route, "basic-open", dummy_action);
    espDefineAction(route, "basic-send", dummy_action);
    espDefineAction(route, "basic-echo", echo_action);
    espDefineAction(route, "basic-ssl", len_action);
    espDefineAction(route, "basic-len", len_action);
    espDefineAction(route, "basic-echo", echo_action);
    espDefineAction(route, "basic-empty", empty_response);
    espDefineAction(route, "basic-big", big_response);
    espDefineAction(route, "basic-frames", frames_response);
    return 0;
}
