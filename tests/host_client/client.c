#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define close_socket close
#endif

enum {
    FRAME_ACK = 2,
    FRAME_ERROR = 3,
    FRAME_PUT_BEGIN = 20,
    FRAME_PUT_CHUNK = 21,
    FRAME_PUT_END = 22
};

static uint32_t read_u32le(const unsigned char *src)
{
    return ((uint32_t) src[0]) |
        ((uint32_t) src[1] << 8) |
        ((uint32_t) src[2] << 16) |
        ((uint32_t) src[3] << 24);
}

static void write_u32le(unsigned char *dst, uint32_t value)
{
    dst[0] = (unsigned char) (value & 0xffu);
    dst[1] = (unsigned char) ((value >> 8) & 0xffu);
    dst[2] = (unsigned char) ((value >> 16) & 0xffu);
    dst[3] = (unsigned char) ((value >> 24) & 0xffu);
}

static int send_all(sock_t sock, const void *buf, size_t len)
{
    const char *p = (const char *) buf;
    while (len > 0) {
        int rc = send(sock, p, (int) len, 0);
        if (rc <= 0) {
            return -1;
        }
        p += rc;
        len -= (size_t) rc;
    }
    return 0;
}

static int recv_all(sock_t sock, void *buf, size_t len)
{
    char *p = (char *) buf;
    while (len > 0) {
        int rc = recv(sock, p, (int) len, 0);
        if (rc <= 0) {
            return -1;
        }
        p += rc;
        len -= (size_t) rc;
    }
    return 0;
}

static int send_frame(sock_t sock, uint8_t type, uint32_t req_id,
    const void *payload, size_t payload_len)
{
    unsigned char hdr[14];

    memcpy(hdr, "RXSH", 4);
    hdr[4] = type;
    hdr[5] = 0;
    write_u32le(hdr + 6, req_id);
    write_u32le(hdr + 10, (uint32_t) payload_len);

    if (send_all(sock, hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (payload_len > 0 && send_all(sock, payload, payload_len) != 0) {
        return -1;
    }
    return 0;
}

static int recv_frame(sock_t sock, uint8_t *type_out, uint32_t *req_id_out,
    char **payload_out, uint32_t *payload_len_out)
{
    unsigned char hdr[14];
    char *payload = NULL;
    uint32_t payload_len;

    if (recv_all(sock, hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (memcmp(hdr, "RXSH", 4) != 0) {
        return -1;
    }

    payload_len = read_u32le(hdr + 10);
    if (payload_len > 1024 * 1024) {
        return -1;
    }

    payload = (char *) calloc(payload_len + 1u, 1u);
    if (payload == NULL) {
        return -1;
    }
    if (payload_len > 0 && recv_all(sock, payload, payload_len) != 0) {
        free(payload);
        return -1;
    }

    *type_out = hdr[4];
    *req_id_out = read_u32le(hdr + 6);
    *payload_out = payload;
    *payload_len_out = payload_len;
    return 0;
}

static sock_t connect_tcp(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *it;
    sock_t sock = (sock_t) -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &result) != 0) {
        return (sock_t) -1;
    }

    for (it = result; it != NULL; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == (sock_t) -1) {
            continue;
        }
        if (connect(sock, it->ai_addr, (socklen_t) it->ai_addrlen) == 0) {
            break;
        }
        close_socket(sock);
        sock = (sock_t) -1;
    }

    freeaddrinfo(result);
    return sock;
}

int main(int argc, char **argv)
{
    sock_t sock;
    FILE *in;
    long file_size;
    uint32_t req_id = 100;
    char begin_payload[1024];
    unsigned char chunk[4096];

    if (argc != 5) {
        fprintf(stderr, "usage: %s <host> <port> <local-file> <remote-path>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "WSAStartup failed\n");
            return 1;
        }
    }
#endif

    in = fopen(argv[3], "rb");
    if (in == NULL) {
        fprintf(stderr, "open input failed: %s\n", strerror(errno));
        return 1;
    }

    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return 1;
    }
    file_size = ftell(in);
    if (file_size < 0 || fseek(in, 0, SEEK_SET) != 0) {
        fclose(in);
        return 1;
    }

    sock = connect_tcp(argv[1], argv[2]);
    if (sock == (sock_t) -1) {
        fprintf(stderr, "connect failed\n");
        fclose(in);
        return 1;
    }

    snprintf(begin_payload, sizeof(begin_payload),
        "path=%s\nsize=%ld\noverwrite=1", argv[4], file_size);
    if (send_frame(sock, FRAME_PUT_BEGIN, req_id, begin_payload,
            strlen(begin_payload)) != 0) {
        fprintf(stderr, "send PUT_BEGIN failed\n");
        fclose(in);
        close_socket(sock);
        return 1;
    }

    {
        uint8_t type;
        uint32_t resp_req;
        uint32_t payload_len;
        char *payload = NULL;

        if (recv_frame(sock, &type, &resp_req, &payload, &payload_len) != 0) {
            fprintf(stderr, "recv PUT_BEGIN reply failed\n");
            fclose(in);
            close_socket(sock);
            return 1;
        }
        printf("begin response type=%u req=%u payload=%s\n",
            (unsigned int) type, (unsigned int) resp_req, payload);
        if (type == FRAME_ERROR) {
            free(payload);
            fclose(in);
            close_socket(sock);
            return 1;
        }
        free(payload);
    }

    for (;;) {
        size_t got = fread(chunk, 1, sizeof(chunk), in);
        if (got > 0 && send_frame(sock, FRAME_PUT_CHUNK, req_id, chunk, got) != 0) {
            fprintf(stderr, "send PUT_CHUNK failed\n");
            fclose(in);
            close_socket(sock);
            return 1;
        }
        if (got < sizeof(chunk)) {
            if (ferror(in)) {
                fprintf(stderr, "read input failed\n");
                fclose(in);
                close_socket(sock);
                return 1;
            }
            break;
        }
    }

    if (send_frame(sock, FRAME_PUT_END, req_id, NULL, 0) != 0) {
        fprintf(stderr, "send PUT_END failed\n");
        fclose(in);
        close_socket(sock);
        return 1;
    }

    {
        uint8_t type;
        uint32_t resp_req;
        uint32_t payload_len;
        char *payload = NULL;

        if (recv_frame(sock, &type, &resp_req, &payload, &payload_len) != 0) {
            fprintf(stderr, "recv PUT_END reply failed\n");
            fclose(in);
            close_socket(sock);
            return 1;
        }
        printf("end response type=%u req=%u payload=%s\n",
            (unsigned int) type, (unsigned int) resp_req, payload);
        free(payload);
        if (type == FRAME_ERROR) {
            fclose(in);
            close_socket(sock);
            return 1;
        }
    }

    fclose(in);
    close_socket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
