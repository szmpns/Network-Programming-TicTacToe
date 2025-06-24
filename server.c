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
    new_player.in_game = -1;
    new_player.symbol = ' ';
    new_player.tcp_sockfd = tcp_sockfd;
    return new_player;
}

void remove_player(Player *players, int *player_count, int tcp_sockfd) {
    for (int i = 0; i < *player_count; i++) {
        if (players[i].tcp_sockfd == tcp_sockfd) {
            // Przesuwamy pozostałych graczy w lewo
            for (int j = i; j < *player_count - 1; j++)
                players[j] = players[j + 1];
            (*player_count)--;
            break;
        }
    }
    
}

Player* find_player_by_name(Player *players, int player_count, const char *name) {
    for (int i = 0; i < player_count; i++) {
        if (strcmp(players[i].name, name) == 0) {
            return &players[i];
        }
    }
    return NULL; // Zwraca NULL jeśli nie znaleziono
}

int check_win_magic_square(char board[9], char player) {
    const int magic[9] = {2, 7, 6,
                          9, 5, 1,
                          4, 3, 8};

    int selected[5];  // maksymalnie 5 pól może mieć gracz
    int count = 0;

    // Zbieramy wartości magiczne zajęte przez danego gracza
    for (int i = 0; i < 9; ++i) {
        if (board[i] == player) {
            selected[count++] = magic[i];
        }
    }

    // Sprawdzamy wszystkie trójki spośród wybranych liczb
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            for (int k = j + 1; k < count; ++k) {
                if (selected[i] + selected[j] + selected[k] == 15) {
                    return 1; // gracz wygrał
                }
            }
        }
    }

    return 0; // brak wygranej
}

void update_score_in_file(const char *player_name, int score) {
    FILE *file = fopen("scores.txt", "r");
    char lines[100][64];
    int found = 0, count = 0;

    // Wczytaj wszystkie linie do pamięci
    while (file && fgets(lines[count], sizeof(lines[count]), file)) {
        char name[32];
        int old_score;
        if (sscanf(lines[count], "%31s %d", name, &old_score) == 2) {
            if (strcmp(name, player_name) == 0) {
                old_score += score; // dodaj do aktualnego wyniku
                snprintf(lines[count], sizeof(lines[count]), "%s %d\n", player_name, old_score);
                found = 1;
            }
        }
        count++;
    }
    if (file) fclose(file);

    // Jeśli nie znaleziono, dodaj nową linię
    if (!found) {
        snprintf(lines[count++], sizeof(lines[0]), "%s %d\n", player_name, score);
    }

    // Zapisz wszystko z powrotem do pliku
    file = fopen("scores.txt", "w");
    if (!file) return;
    for (int i = 0; i < count; i++) {
        fputs(lines[i], file);
    }
    fclose(file);
}

void clear_game(Game *game) {
    game->player1 = -1;
    game->player2 = -1;
    memset(game->board, ' ', sizeof(game->board));
    game->turn = 0;
    game->finished = 0;
}

Player *players;
Game *games;
int *player_count;
int *game_count;

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

    games = mmap(NULL, sizeof(Game) * 5, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    game_count = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *game_count = 0;

    //inicjalizacja gier
    for (int i = 0; i < 5; i++) {
        clear_game(&games[i]);
    }

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

                // ...po odebraniu imienia gracza:
                int name_taken = 0;
                for (int i = 0; i < *player_count; i++) {
                    if (strcmp(players[i].name, buffer) == 0) {
                        name_taken = 1;
                        break;
                    }
                }
                if (name_taken) {
                    const char *msg = "Nazwa gracza jest już zajęta. Wybierz inną.\n";
                    send(client_sock, msg, strlen(msg), 0);
                    close(client_sock);
                    exit(0);
                }
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
                    char player1_name[32], player2_name[32], player_name[32];
                    char board[9] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
                    
                    printf("Otrzymano od klienta: %s\n", buffer);
                    
                    int index;
                    char symbol;

                    //poprawić w tym movie, eby wysyłało do obu graczy bo narazie tylko odsyła do tego co wysłał

                    if (sscanf(buffer, "MOVE %d %31s", &index, player_name) == 2) {
                        if(find_player_by_name(players, *player_count, player_name)->in_game == -1) {
                            send(client_sock, "Nie jesteś w grze!\n", 21, 0);
                            continue;
                        }
                        if (index >= 1 && index <= 9) {
                           int game_id = find_player_by_name(players, *player_count, player_name)->in_game;
                           symbol = find_player_by_name(players, *player_count, player_name)->symbol;

                            if(games[game_id].board[index - 1] != ' ') {
                                send(client_sock, "To pole jest już zajęte!\n", 26, 0);
                            } else if((games[game_id].turn % 2 == 0 && symbol == 'X') || 
                                       (games[game_id].turn % 2 == 1 && symbol == 'O')) {
                                games[game_id].board[index - 1] = symbol;
                                games[game_id].turn++;
                                
                                // Sformatuj planszę do stringa
                                char board_msg[BUFFER_SIZE];
                                snprintf(board_msg, sizeof(board_msg),
                                    "%c|%c|%c\n- - -\n%c|%c|%c\n- - -\n%c|%c|%c\n",
                                    games[game_id].board[0], games[game_id].board[1], games[game_id].board[2],
                                    games[game_id].board[3], games[game_id].board[4], games[game_id].board[5],
                                    games[game_id].board[6], games[game_id].board[7], games[game_id].board[8]
                                );

                                // tutaj check win/draw
                                if (check_win_magic_square(games[game_id].board, symbol)) {
                                    char win_msg[BUFFER_SIZE];
                                    snprintf(win_msg, sizeof(win_msg), "Gratulacje %s! Wygrałeś!\n", player_name);
                                    send(games[game_id].player1, win_msg, strlen(win_msg), 0);
                                    games[game_id].finished = 1;
                                    find_player_by_name(players, *player_count, player_name)->in_game = -1;
                                    find_player_by_name(players, *player_count, player_name)->symbol = ' ';
                                    find_player_by_name(players, *player_count, player_name)->score++;
                                    update_score_in_file(player_name, 1);
                                    clear_game(&games[game_id]);
                                } else if (games[game_id].turn == 9) {
                                    const char *draw_msg = "Remis!\n";
                                    send(games[game_id].player1, draw_msg, strlen(draw_msg), 0);
                                    games[game_id].finished = 1;
                                    find_player_by_name(players, *player_count, player_name)->in_game = -1;
                                    find_player_by_name(players, *player_count, player_name)->symbol = ' ';
                                    find_player_by_name(players, *player_count, player_name)->score++;
                                    clear_game(&games[game_id]);
                                }
                                //nie wiem dla czego to wysyła dwa razy do jednego gracza
                                //tutaj jeszcze zmienic tak zeby kasowalo dane z drugiego gracza

                                send(games[game_id].player1, board_msg, strlen(board_msg), 0);
                                //send(games[game_id].player2, board_msg, strlen(board_msg), 0);

                           } else {
                               send(client_sock, "Nie twoja kolej!\n", 17, 0);
                               continue;
                           }
                           
                        }else{
                            send(client_sock, "Nieprawidłowy ruch. Wybierz pole od 1 do 9.\n", 44, 0);
                            continue;
                        }
                    } else if(strncmp(buffer, "LIST", 4) == 0) {
                        char player_list[BUFFER_SIZE] = "Aktywni gracze:\n";
                        for (int i = 0; i < *player_count; i++) {
                            strcat(player_list, players[i].name);
                            char score_str[12];
                            snprintf(score_str, sizeof(score_str), " %d", players[i].score);
                            strcat(player_list, score_str);
                            strcat(player_list, "\n");
                        }
                        send(client_sock, player_list, strlen(player_list), 0);
                    } else if(sscanf(buffer, "CHALLENGE %31s %31s", player1_name, player2_name) == 2){
                        int found = 0;
                        printf("Wyzwanie od %s do %s\n", player1_name, player2_name);
                        if(*player_count < 2) {
                            send(client_sock, "Za mało graczy do rozpoczęcia gry.\n", 36, 0);
                            continue;
                        }

                        if(strcmp(player1_name, player2_name) == 0) {
                            send(client_sock, "Nie możesz wyzwać samego siebie.\n", 34, 0);
                            continue;
                        }


                        int k,m;
                        for (int i = 0; i < *player_count; i++) {
                            if (strcmp(players[i].name, player1_name) == 0 && players[i].in_game == -1) {
                                players[i].symbol = 'X';
                                players[i].in_game = *game_count;
                                games[*game_count].player1 = players[i].tcp_sockfd;
                                k = i;
                                found += 1;
                            } else if (strcmp(players[i].name, player2_name) == 0 && players[i].in_game == -1) {
                                players[i].symbol = 'O';
                                players[i].in_game = *game_count;
                                games[*game_count].player2 = players[i].tcp_sockfd;
                                m = i;
                                found += 1;
                            }
                        }
                        if (found < 2) {
                            send(client_sock, "Gracz nie znaleziony lub obecnie podczas rozgrywki.\n", 53, 0);
                            clear_game(&games[*game_count]);
                            players[k].in_game = -1;
                            players[m].in_game = -1;
                            players[k].symbol = ' ';
                            players[m].symbol = ' ';
                        } else {
                            char start_msg[BUFFER_SIZE];
                            snprintf(start_msg, sizeof(start_msg), "Rozpoczęto grę! Zaczyna gracz %s\n", player1_name);
                            send(client_sock, start_msg, strlen(start_msg), 0);
                        }
                    }else if(strcmp(buffer, "SCORE") == 0){
                        FILE *file = fopen("scores.txt", "r");
                        char score_list[BUFFER_SIZE] = "Wyniki graczy:\n";
                        char line[64];
                        if (file) {
                            while (fgets(line, sizeof(line), file)) {
                                strcat(score_list, line);
                            }
                            fclose(file);
                        } else {
                            strcat(score_list, "Brak wyników.\n");
                        }
                        send(client_sock, score_list, strlen(score_list), 0);
                    } 
                    else {
                        send(client_sock, "Nieznana komenda. Dostępne komendy to: MOVE <pole>, LIST, CHALLENGE <gracz>\n", 64, 0);
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
