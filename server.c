#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/mman.h>

#define MULTICAST_GROUP "239.0.0.1"
#define MULTICAST_PORT 12345
#define TCP_PORT 54321
#define BUFFER_SIZE 1024

typedef struct {
    char name[32];
    struct sockaddr_in addr;
    int score;
    int in_game;
    char symbol; 
    int tcp_sockfd;
} Player;

typedef struct {
    int player1; 
    int player2;
    char board[9];
    int turn;
    int finished;
} Game;

void sigchld_handler(int signo) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

Player add_player(char *name, struct sockaddr_in *addr, int tcp_sockfd) {
    Player new_player;
    strncpy(new_player.name, name, strlen(name));
    new_player.name[strlen(name)] = '\0';
    new_player.addr = *addr;
    new_player.score = 0;
    new_player.in_game = 0;
    new_player.symbol = ' ';
    new_player.tcp_sockfd = tcp_sockfd;
    return new_player;
}

void remove_player(Player *players, int *player_count, int tcp_sockfd) {
    for (int i = 0; i < *player_count; i++) {
        if (players[i].tcp_sockfd == tcp_sockfd) {
            for (int j = i; j < *player_count - 1; j++) {
                players[j] = players[j + 1];
            }
            (*player_count)--;
            break;
        }
    }
    
}

Player *players;
int *player_count;

int main() {
    int udp_sock, tcp_sock;
    struct sockaddr_in mcast_addr, client_addr, tcp_addr;
    socklen_t addrlen = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    signal(SIGCHLD, sigchld_handler);

    // --- UDP multicast socket ---
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) { perror("UDP socket"); exit(1); }

    int reuse = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(MULTICAST_PORT);
    mcast_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(udp_sock, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr)) < 0) {
        perror("bind UDP");
        exit(1);
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt IP_ADD_MEMBERSHIP");
        exit(1);
    }

    // --- TCP socket ---
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) { perror("TCP socket"); exit(1); }

    setsockopt(tcp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&tcp_addr, 0, sizeof(tcp_addr));
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_addr.s_addr = INADDR_ANY;
    tcp_addr.sin_port = htons(TCP_PORT);

    if (bind(tcp_sock, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr)) < 0) {
        perror("bind TCP");
        exit(1);
    }

    if (listen(tcp_sock, 10) < 0) {
        perror("listen");
        exit(1);
    }

    printf("Serwer gotowy. Nasłuch multicast %s:%d i TCP %d\n",
           MULTICAST_GROUP, MULTICAST_PORT, TCP_PORT);

    fd_set readfds;
    int maxfd = (udp_sock > tcp_sock) ? udp_sock : tcp_sock;

    //Współdzielona pamięć dla kazdego forka
    players = mmap(NULL, sizeof(Player) * 10, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    player_count = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *player_count = 0;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(udp_sock, &readfds);
        FD_SET(tcp_sock, &readfds);

        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            exit(1);
        }

        // --- Multicast UDP packet ---
        if (FD_ISSET(udp_sock, &readfds)) {
            struct sockaddr_in sender_addr;
            socklen_t sender_len = sizeof(sender_addr);
            int n = recvfrom(udp_sock, buffer, BUFFER_SIZE - 1, 0,
                             (struct sockaddr*)&sender_addr, &sender_len);
            if (n < 0) {
                perror("recvfrom");
                continue;
            }
            buffer[n] = '\0';
            printf("Multicast od %s: %s\n", inet_ntoa(sender_addr.sin_addr), buffer);

            // Odpowiedź: "Witaj"
            const char *msg = "Witaj";
            sendto(udp_sock, msg, strlen(msg), 0,
                   (struct sockaddr*)&sender_addr, sender_len);
            printf("Odesłano 'Witaj' do %s\n", inet_ntoa(sender_addr.sin_addr));
        }

        // --- Nowe połączenie TCP ---
        if (FD_ISSET(tcp_sock, &readfds)) {
            struct sockaddr_in tcp_client;
            socklen_t tcp_client_len = sizeof(tcp_client);
            int client_sock = accept(tcp_sock, (struct sockaddr*)&tcp_client, &tcp_client_len);
            if (client_sock < 0) {
                perror("accept");
                continue;
            }

            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                close(client_sock);
                continue;
            }

            if (pid == 0) {
                // Sprawdź, czy serwer jest pełny
                if(*player_count >= 10) {
                    const char *msg = "Serwer pełny. Spróbuj ponownie później.\n";
                    send(client_sock, msg, strlen(msg), 0);
                    close(client_sock);
                    exit(0);
                }

                // Proces potomny - obsługuje klienta TCP
                close(tcp_sock);
                printf("Nowy klient TCP: %s:%d\n",
                       inet_ntoa(tcp_client.sin_addr), ntohs(tcp_client.sin_port));
                


                // Odbierz imię gracza
                ssize_t n = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);
                if (n <= 0) {
                    perror("recv");
                    close(client_sock);
                    exit(1);
                }
                buffer[n] = '\0';
                
                //Teraz dodajesz gracza na podstawie danych wpisanych przez klienta
                players[*player_count] = add_player(buffer, &tcp_client, client_sock);
                (*player_count)++;

                // --- aktywni gracze ---
                // const char *info = "Lista aktywnych graczy: [tu przykładowa lista]\n";
                
                while (1) {
                    ssize_t n = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);
                    if (n <= 0) {
                        if (n == 0){
                            remove_player(players, player_count, client_sock);
                            printf("Klient rozłączył się\n");
                        }
                        else
                            perror("recv");
                        break;
                    }
                    buffer[n] = '\0';
                    char field;
                    char sign;
                    char board[9] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
                    
                    printf("Otrzymano od klienta: %s\n", buffer);
                    
                    int index;
                    char symbol;
                    if (sscanf(buffer, "MOVE %d %c", &index, &symbol) == 2) {
                        if (index >= 1 && index <= 9 && board[index - 1] != 'X' && board[index - 1] != 'O') {
                            board[index - 1] = symbol;
                            send(client_sock, board, strlen(board), 0);
                        } 
                    } else if(sscanf(buffer, "LIST") == 0) {
                        char player_list[BUFFER_SIZE] = "Aktywni gracze:\n";
                        for (int i = 0; i < *player_count; i++) {
                            strcat(player_list, players[i].name);
                            strcat(player_list, "\n");
                        }
                        send(client_sock, player_list, strlen(player_list), 0);
                    } else {
                        send(client_sock, "Nieznana komenda\n", 17, 0);
                    }
                }

                close(client_sock);
                exit(0);
            }

            // Proces macierzysty
            close(client_sock);
        }
    }

    close(udp_sock);
    close(tcp_sock);
    return 0;
}
