#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>


#define MULTICAST_GROUP "239.0.0.1"
#define MULTICAST_PORT 12345
#define TCP_PORT 54321
#define BUFFER_SIZE 1024

int main() {
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
    inet_pton(AF_INET, MULTICAST_GROUP, &mcast_addr.sin_addr);

    const char* hello = "Cześć serwer, chce zagrać w gierke";
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


    pid_t pid = fork();
if (pid == 0) {
    // Proces potomny: odbieranie wiadomości od serwera
    while (1) {
        int n = recv(tcp_sock, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            printf("Serwer zakończył połączenie\n");
            exit(0);
        }
        buffer[n] = '\0';
        printf("\n[Serwer]: %s\n", buffer);
        printf("Wpisz wiadomość do serwera (q aby wyjść): ");
        fflush(stdout);
    }
    exit(0);
} else {
    // Proces macierzysty: wysyłanie wiadomości do serwera
    printf("Wpisz wiadomość do serwera (q aby wyjść): ");
    while (1) {
        if (fgets(buffer, sizeof(buffer), stdin) == NULL)
            break;

        if (buffer[0] == 'q' && (buffer[1] == '\n' || buffer[1] == '\0'))
            break;

        send(tcp_sock, buffer, strlen(buffer), 0);
    }
    close(tcp_sock);
    kill(pid, SIGKILL); // zakończ proces potomny
    return 0;
}
}
