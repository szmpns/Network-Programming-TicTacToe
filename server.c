#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

#define MAXFD 64

#define MULTICAST_GROUP "239.0.0.1"
#define MULTICAST_PORT 12345
#define SERVER_PORT 23456
#define MAX_PLAYERS 20
#define MAX_GAMES 10
#define BUF_SIZE 256
#define SCORE_FILE "scores.txt"

// Struktura gracza, która przechowuje informacje o graczu
// oraz jego adresie sieciowym i wyniku
typedef struct {
    char name[32];
    struct sockaddr_in addr;
    int score;
    int in_game; // -1 jeśli nie gra, w przeciwnym razie indeks gry
    char symbol; // 'X' lub 'O'
} Player;

typedef struct {
    int player1; // indeks w tablicy players
    int player2;
    char board[3][3]; // plansza
    int turn; // indeks gracza, który wykonuje ruch
    int finished; // 0 - trwa, 1 - zakończona
} Game;

Player players[MAX_PLAYERS];
int player_count = 0;
Game games[MAX_GAMES];
int game_count = 0;

// Funkcja dodająca gracza do listy graczy
// Sprawdza, czy gracz o danej nazwie już istnieje
// Jeśli nie, dodaje go do listy i inicjalizuje jego wynik na 0
// Jeśli gracz już istnieje, nie dodaje go ponownie
void add_player(const char *name, struct sockaddr_in *client_addr) {
    for (int i = 0; i < player_count; i++) {
        if (strcmp(players[i].name, name) == 0) return;
    }
    if (player_count < MAX_PLAYERS) {
        strncpy(players[player_count].name, name, 31);
        players[player_count].name[31] = '\0';
        players[player_count].addr = *client_addr;
        players[player_count].score = 0;
        players[player_count].in_game = -1;
        players[player_count].symbol = 0;
        player_count++;
    }
}

// Znajdź indeks gracza po nazwie
int find_player(const char *name) {
    for (int i = 0; i < player_count; i++) {
        if (strcmp(players[i].name, name) == 0) return i;
    }
    return -1;
}

// Rozpocznij grę między dwoma graczami
int start_game(int idx1, int idx2) {
    if (game_count >= MAX_GAMES) return -1;
    for (int i = 0; i < MAX_GAMES; i++) {
        if (games[i].finished || games[i].player1 == -1) {
            games[i].player1 = idx1;
            games[i].player2 = idx2;
            memset(games[i].board, ' ', sizeof(games[i].board));
            games[i].turn = idx1;
            games[i].finished = 0;
            players[idx1].in_game = i;
            players[idx2].in_game = i;
            players[idx1].symbol = 'X';
            players[idx2].symbol = 'O';
            return i;
        }
    }
    return -1;
}

// Sprawdź zwycięstwo
char check_winner(char board[3][3]) {
    for (int i = 0; i < 3; i++) {
        if (board[i][0] != ' ' && board[i][0] == board[i][1] && board[i][1] == board[i][2]) return board[i][0];
        if (board[0][i] != ' ' && board[0][i] == board[1][i] && board[1][i] == board[2][i]) return board[0][i];
    }
    if (board[0][0] != ' ' && board[0][0] == board[1][1] && board[1][1] == board[2][2]) return board[0][0];
    if (board[0][2] != ' ' && board[0][2] == board[1][1] && board[1][1] == board[2][0]) return board[0][2];
    return 0;
}

// Sprawdź remis
int is_draw(char board[3][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (board[i][j] == ' ') return 0;
    return 1;
}

// Obsłuż ruch gracza
void handle_move(int sockfd, int player_idx, int row, int col, struct sockaddr_in *mcast_addr) {
    int game_idx = players[player_idx].in_game;
    if (game_idx == -1) return;
    Game *g = &games[game_idx];
    if (g->finished) return;
    if (g->turn != player_idx) return;
    if (row < 0 || row > 2 || col < 0 || col > 2) return;
    if (g->board[row][col] != ' ') return;

    g->board[row][col] = players[player_idx].symbol;
    char winner = check_winner(g->board);
    int draw = is_draw(g->board);

    int other_idx = (g->player1 == player_idx) ? g->player2 : g->player1;

    char msg[BUF_SIZE];
    if (winner) {
        snprintf(msg, sizeof(msg), "GAME_OVER %s %s %c", players[g->player1].name, players[g->player2].name, winner);
        sendto(sockfd, msg, strlen(msg), 0, (struct sockaddr *)mcast_addr, sizeof(*mcast_addr));
        if (players[g->player1].symbol == winner) players[g->player1].score++;
        else players[g->player2].score++;
        g->finished = 1;
        players[g->player1].in_game = -1;
        players[g->player2].in_game = -1;
    } else if (draw) {
        snprintf(msg, sizeof(msg), "GAME_OVER %s %s D", players[g->player1].name, players[g->player2].name);
        sendto(sockfd, msg, strlen(msg), 0, (struct sockaddr *)mcast_addr, sizeof(*mcast_addr));
        g->finished = 1;
        players[g->player1].in_game = -1;
        players[g->player2].in_game = -1;
    } else {
        g->turn = other_idx;
        snprintf(msg, sizeof(msg), "BOARD %s %s %c %c%c%c%c%c%c%c%c%c%c",
            players[g->player1].name, players[g->player2].name,
            players[g->turn].symbol,
            g->board[0][0], g->board[0][1], g->board[0][2],
            g->board[1][0], g->board[1][1], g->board[1][2],
            g->board[2][0], g->board[2][1], g->board[2][2]);
        sendto(sockfd, msg, strlen(msg), 0, (struct sockaddr *)mcast_addr, sizeof(*mcast_addr));
    }
}

// Obsłuż żądanie rozpoczęcia gry
void handle_challenge(int sockfd, int challenger_idx, const char *opponent_name, struct sockaddr_in *mcast_addr) {
    int opponent_idx = find_player(opponent_name);
    if (opponent_idx == -1 || players[opponent_idx].in_game != -1 || players[challenger_idx].in_game != -1) return;
    int game_idx = start_game(challenger_idx, opponent_idx);
    if (game_idx == -1) return;
    char msg[BUF_SIZE];
    snprintf(msg, sizeof(msg), "GAME_START %s %s", players[challenger_idx].name, players[opponent_idx].name);
    sendto(sockfd, msg, strlen(msg), 0, (struct sockaddr *)mcast_addr, sizeof(*mcast_addr));
}

// Funkcja zapisująca wyniki graczy do pliku
void save_scores(){
    FILE *file = fopen(SCORE_FILE, "w");
    if (file == NULL) {
        perror("Could not open score file");
        return;
    }
    for(int i = 0; i < player_count; i++) {
        fprintf(file, "%s %d\n", players[i].name, players[i].score);
    }
    fclose(file);
}

// Wysyłanie listy graczy do klienta
void send_player_list(int sockfd, struct sockaddr_in *mcast_addr) {
    char buffer[BUF_SIZE];
    int len = 0;
    for(int i = 0; i < player_count; i++) {
        len += snprintf(buffer + len, BUF_SIZE - len, "%s %d\n", players[i].name, players[i].score);
        if (len >= BUF_SIZE - 1) break;
    }
    sendto(sockfd, buffer, len, 0, (struct sockaddr *)mcast_addr, sizeof(*mcast_addr));
}


// Odebranie wyniku gry od klienta
// Aktualizuje wynik gracza i zapisuje wyniki do pliku
void handle_game_result(int sockfd, struct sockaddr_in *client_addr, const char *name, int score) {
    for(int i = 0; i < player_count; i++) {
        if (strcmp(players[i].name, name) == 0) {
            players[i].score += score;
            save_scores();
            return;
        }
    }
    // Jeśli gracz nie został znaleziony, dodaj go
    add_player(name, client_addr);
    players[player_count - 1].score = score;
    save_scores();
}

int daemon_init(const char *pname, int facility, uid_t uid, int socket)
{
	int		i, p;
	pid_t	pid;

	if ( (pid = fork()) < 0)
		return (-1);
	else if (pid)
		exit(0);

	if (setsid() < 0)
		return (-1);

	signal(SIGHUP, SIG_IGN);
	if ( (pid = fork()) < 0)
		return (-1);
	else if (pid)
		exit(0);

	chdir("/tmp");

	for (i = 0; i < MAXFD; i++){
		if(socket != i )
			close(i);
	}

	p= open("/dev/null", O_RDONLY);
	open("/dev/null", O_RDWR);
	open("/dev/null", O_RDWR);

	openlog(pname, LOG_PID, facility);
	
	setuid(uid);
	
	return (0);
}

int main(int argc, char **argv) {
    int sockfd;
    struct sockaddr_in server_addr, mcast_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[BUF_SIZE];

    // Tworzenie gniazda UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0)
        perror("setsockopt failed");

    // Ustawienie adresu serwera (bind na porcie multicastowym)
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(MULTICAST_PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Dołączenie do grupy multicastowej
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt IP_ADD_MEMBERSHIP");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Przygotowanie adresu multicastowego do wysyłania odpowiedzi
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_addr.s_addr = inet_addr(MULTICAST_GROUP);
    mcast_addr.sin_port = htons(MULTICAST_PORT);

    printf("Server is running on multicast %s:%d\n", MULTICAST_GROUP, MULTICAST_PORT);

    // Inicjalizacja gier
    for (int i = 0; i < MAX_GAMES; i++) {
        games[i].player1 = -1;
        games[i].player2 = -1;
        games[i].finished = 1;
    }

    daemon_init(argv[0], LOG_USER, 1000, sockfd);
    syslog (LOG_NOTICE, "Program started by User %d", getuid ());
	syslog (LOG_INFO,"Waiting for clients ... ");

    while (1) {
        int n = recvfrom(sockfd, buffer, BUF_SIZE - 1, 0, (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) continue;
        buffer[n] = '\0';

        // Protokół: "ADD_PLAYER <nick>", "CHALLENGE <nick>", "MOVE <nick> <row> <col>"
        if (strncmp(buffer, "ADD_PLAYER ", 11) == 0) {
            add_player(buffer + 11, &client_addr);
            // Wysyłaj listę graczy do wszystkich przez multicast
            send_player_list(sockfd, &mcast_addr);
        } else if (strncmp(buffer, "CHALLENGE ", 10) == 0) {
            char challenger[32], opponent[32];
            sscanf(buffer + 10, "%31s %31s", challenger, opponent);
            int idx = find_player(challenger);
            if (idx != -1) handle_challenge(sockfd, idx, opponent, &mcast_addr);
        } else if (strncmp(buffer, "MOVE ", 5) == 0) {
            char name[32];
            int row, col;
            sscanf(buffer + 5, "%31s %d %d", name, &row, &col);
            int idx = find_player(name);
            if (idx != -1) handle_move(sockfd, idx, row, col, &mcast_addr);
        }
        // ...obsługa innych komend...
    }

    close(sockfd);
    return 0;
}



