#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define MULTICAST_GROUP "239.0.0.1"
#define MULTICAST_PORT 12345
#define BUF_SIZE 256

// Funkcja odbiera wiadomości z serwera (dla multicast)
void receive_messages(int sockfd) {
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    char buffer[BUF_SIZE];

    while (1) {
        int n = recvfrom(sockfd, buffer, BUF_SIZE - 1, 0, (struct sockaddr *)&src_addr, &addr_len);
        if (n > 0) {
            buffer[n] = '\0';
            printf("[Server] %s\n", buffer);
        }
    }
}

int main() {
    int sockfd;
    struct sockaddr_in addr;
    struct ip_mreq mreq;
    char buffer[BUF_SIZE];
    char name[32];

    // Gniazdo UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Ustawienie adresu klienta (dla multicast)
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(MULTICAST_PORT);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    // Dołączenie do grupy multicastowej
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt IP_ADD_MEMBERSHIP");
        exit(EXIT_FAILURE);
    }

    // Pobierz nazwę gracza
    printf("Enter your player name: ");
    fgets(name, sizeof(name), stdin);
    name[strcspn(name, "\n")] = 0;
    // Wysyłanir do serwera informację o graczu (ADD_PLAYER)
    snprintf(buffer, sizeof(buffer), "ADD_PLAYER %s", name);
    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_addr.s_addr = inet_addr(MULTICAST_GROUP);
    mcast_addr.sin_port = htons(MULTICAST_PORT);

    sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));

    // Proces potomny do odbioru wiadomości
    if (fork() == 0) {
        receive_messages(sockfd);
        exit(0);
    }

    while (1) {
        printf("\nAvailable commands:\n");
        printf("1. Challenge a player: CHALLENGE <name>\n");
        printf("2. Make a move: MOVE <row> <column>\n");
        printf("3. Exit the game: EXIT\n> ");

        fgets(buffer, sizeof(buffer), stdin);
        buffer[strcspn(buffer, "\n")] = 0;

        if (strncmp(buffer, "CHALLENGE ", 10) == 0) {
            // CHALLENGE <nazwa_przeciwnika>
            char cmd[BUF_SIZE];
            snprintf(cmd, sizeof(cmd), "CHALLENGE %s %s", name, buffer + 10);
            sendto(sockfd, cmd, strlen(cmd), 0, (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
        } else if (strncmp(buffer, "MOVE ", 5) == 0) {
            // MOVE <wiersz> <kolumna>
            int row, col;
            if (sscanf(buffer + 5, "%d %d", &row, &col) == 2) {
                char cmd[BUF_SIZE];
                snprintf(cmd, sizeof(cmd), "MOVE %s %d %d", name, row, col);
                sendto(sockfd, cmd, strlen(cmd), 0, (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
            }
        } else if (strcmp(buffer, "EXIT") == 0) {
            break;
        }
    }

    close(sockfd);
    return 0;
}