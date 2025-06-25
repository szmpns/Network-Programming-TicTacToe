#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <netdb.h>


#define MULTICAST_PORT 12345
#define TCP_PORT 54321
#define BUFFER_SIZE 1024

int main(int argc, char **argv) {

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <nazwa-FQDN-serwera>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(argv[1], NULL, &hints, &res) != 0) {
        perror("getaddrinfo failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in *resolved_addr = (struct sockaddr_in *)res->ai_addr;
    char multicast_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(resolved_addr->sin_addr), multicast_ip, sizeof(multicast_ip));
    freeaddrinfo(res);

    printf("Resolved multicast IP: %s\n", multicast_ip);
    
    int udp_sock, tcp_sock;
    struct sockaddr_in mcast_addr, from_addr;
    socklen_t from_len = sizeof(from_addr);
    char buffer[BUFFER_SIZE];

    // UDP multicast send
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) { perror("socket"); exit(1); }

    if(setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) { perror("setsockopt"); exit(1); }

    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(MULTICAST_PORT);
    inet_pton(AF_INET, multicast_ip, &mcast_addr.sin_addr);

    const char* hello = "Hello from remote";
    sendto(udp_sock, hello, strlen(hello), 0, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
    printf("Wysłano do grupy multicast: %s\n", hello);

    // Odbierz odpowiedź i ustal IP serwera
    recvfrom(udp_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&from_addr, &from_len);
    buffer[BUFFER_SIZE - 1] = '\0';
    printf("Odebrano od serwera: %s\n", buffer);

    char serwer_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from_addr.sin_addr, serwer_ip, sizeof(serwer_ip));
    printf("Adres IP serwera: %s\n", serwer_ip);

    close(udp_sock);

    // TCP connect
    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in tcp_addr;
    memset(&tcp_addr, 0, sizeof(tcp_addr));
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_port = htons(TCP_PORT);
    inet_pton(AF_INET, serwer_ip, &tcp_addr.sin_addr);

    sleep(1);

    if (connect(tcp_sock, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr)) < 0) {
        perror("connect");
        exit(1);
    }

    printf("Połączono z serwerem TCP\n");
    //Wyślij dane gracza do serwera
    char player_name[32];
    printf("Podaj swoje imię: ");
    if (fgets(player_name, sizeof(player_name), stdin) == NULL) {
        perror("fgets");
        close(tcp_sock);
        exit(1);
    }
    player_name[strcspn(player_name, "\n")] = '\0'; // Usuń znak nowej linii
    send(tcp_sock, player_name, strlen(player_name), 0);

    printf("Jaką akcję chcesz wykonać (q aby wyjść, HELP aby uzyskac pomoc): ");

    pid_t pid = fork();


if (pid == 0) {
    // Proces potomny: odbieranie wiadomości od serwera
    while (1) {
        memset(buffer, 0, sizeof(buffer)); 
        int n = recv(tcp_sock, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            printf("Serwer zakończył połączenie\n");
            kill(getppid(), SIGKILL);
            exit(0);
        }
        buffer[n] = '\0';
        printf("\n[Serwer]: \n%s", buffer);
        printf("Jaką akcję chcesz wykonać (q aby wyjść): ");
        fflush(stdout);
    }
    exit(0);
} else {
    // Proces macierzysty: wysyłanie wiadomości do serwera
    while (1) {

        if (fgets(buffer, sizeof(buffer), stdin) == NULL)
            break;

        buffer[strcspn(buffer, "\n")] = '\0'; // Usuń znak nowej linii

        if (buffer[0] == 'q' && (buffer[1] == '\n' || buffer[1] == '\0'))
            break;

        char msg[BUFFER_SIZE];
        if(strncmp(buffer, "MOVE ", 5) == 0){
            char move_arg[BUFFER_SIZE];
            strncpy(move_arg, buffer + 5, sizeof(move_arg));
            move_arg[sizeof(move_arg) - 1] = '\0';
            // Usuń spacje z końca
            for(int i = strlen(move_arg) - 1; i >= 0 && move_arg[i] == ' '; i--) {
                move_arg[i] = '\0';
            }
            snprintf(msg, sizeof(msg), "MOVE %s %s", move_arg, player_name);
            send(tcp_sock, msg, strlen(msg), 0);
        } else if(strncmp(buffer, "CHALLENGE ", 10) == 0) {
            char chall_arg[BUFFER_SIZE];
            strncpy(chall_arg, buffer + 10, sizeof(chall_arg));
            chall_arg[sizeof(chall_arg) - 1] = '\0';
            for(int i = strlen(chall_arg) - 1; i >= 0 && chall_arg[i] == ' '; i--) {
                chall_arg[i] = '\0';
            }
            snprintf(msg, sizeof(msg), "CHALLENGE %s %s", chall_arg, player_name);
            send(tcp_sock, msg, strlen(msg), 0);
        } else if(strncmp(buffer, "LIST", 4) == 0) {
            snprintf(msg, sizeof(msg), "LIST");
            send(tcp_sock, msg, strlen(msg), 0);
        } else if(strncmp(buffer, "SCORE", 5) == 0) {
            snprintf(msg, sizeof(msg), "SCORE");
            send(tcp_sock, msg, strlen(msg), 0);
        } else if (strncmp(buffer, "HELP", 4) == 0) {
            printf("1) MOVE <pole>\n2) LIST\n3) CHALLENGE <gracz>\n4) SCORE\n");
            continue;
        } else {
            printf("Nieznana komenda.\n");
            continue;
        }

        
    }
    close(tcp_sock);
    kill(pid, SIGKILL); // zakończ proces potomny
    return 0;
}
}
