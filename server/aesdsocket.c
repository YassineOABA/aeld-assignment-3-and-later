#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <arpa/inet.h>
#include <fcntl.h>

volatile sig_atomic_t g_caught_signal = 0;
int daemon_mode = 0;

void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        g_caught_signal = 1;
        printf("Caught signal, exiting\n");
		syslog(LOG_INFO, "Caught signal, exiting");
    }
}

void deamon (){

	pid_t pid = fork();
	if (pid < 0) {
        syslog(LOG_ERR, "fork");
        exit(1);
    }

	if (pid > 0) {
        exit(0);
    }

	if (setsid() < 0) {
        syslog(LOG_ERR, "setsid");
        exit(1);
    }

	if ((chdir("/")) < 0) {
        syslog(LOG_ERR, "chdir");
        exit(1);
    }
	
	int fd = open("/dev/null", O_RDWR);
    if (fd != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) {
            close(fd);
        }
    }
}

int main(int argc, char** argv) {
	
	int status, srvsockfd, clsockfd;
	struct sockaddr addrcl;
	struct addrinfo hints;
	struct addrinfo* srvaddrinfo;	
	struct sigaction sa;

	openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

	if(argc == 2 && strcmp(argv[1], "-d") == 0){
		printf("Deamon mode is enabled\n");
		syslog(LOG_INFO, "Deamon mode is enabled");
		daemon_mode = 1;
	}
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Clears SA_RESTART so blocking calls return EINTR immediately
    
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        syslog(LOG_ERR, "Error registering SIGINT");
        exit(1);
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "Error registering SIGTERM");
        exit(1);
    }
		
	memset(&hints, 0, sizeof(hints));
	
	hints.ai_family = AF_INET;
	hints.ai_socktype= SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	
	if((status = getaddrinfo(NULL, "9000", &hints, &srvaddrinfo )) != 0) {
		syslog(LOG_ERR, "getaddrinfo");
		exit(1);
	}

	if((srvsockfd = socket(srvaddrinfo->ai_family, srvaddrinfo->ai_socktype, srvaddrinfo->ai_protocol)) == -1) {
		syslog(LOG_ERR, "socket");
		exit(1);
	}

	if(bind(srvsockfd, srvaddrinfo->ai_addr, srvaddrinfo->ai_addrlen)) {
		syslog(LOG_ERR, "bind");
		exit(1);
	}	
	
	freeaddrinfo(srvaddrinfo);

	if(daemon_mode) {
		deamon();
	}
	if((listen(srvsockfd, 1)) == -1) {
		syslog(LOG_ERR, "listen");
		exit(1);
	}
	syslog(LOG_INFO, "Listening on port 9000"); 	

	while(g_caught_signal == 0) {
		socklen_t addrlen = sizeof(addrcl);
		if((clsockfd = accept(srvsockfd, &addrcl, &addrlen)) == -1) {
			if (g_caught_signal) {
                break; // Exit cleanly if signal was caught
            }
			syslog(LOG_ERR, "accept");
			printf("Failed at accept\n");
			continue;
		}
		char client_ip[INET6_ADDRSTRLEN];
		struct sockaddr_in *s = (struct sockaddr_in *)&addrcl;
		inet_ntop(AF_INET, &s->sin_addr, client_ip, sizeof(client_ip));
		syslog(LOG_INFO, "Accepted connection from %s", client_ip);
		
		FILE *datafile = fopen("/var/tmp/aesdsocketdata", "a+");
		if(datafile == NULL) {
			syslog(LOG_ERR, "fopen");
			printf("Failed at fopen 1\n");
			exit(1);
		}
		char* buffer = malloc(1024);
		if(buffer == NULL) {
			syslog(LOG_ERR, "malloc");
			printf("Failed at malloc\n");
			fclose(datafile);
			exit(1);
		}
		size_t capacity = 1024;
		size_t newcapacity;
		size_t currentlength = 0;
		ssize_t receivedbytes;
		syslog(LOG_INFO, "Receiving data over network...");
		while(1) {
			if(currentlength >= (capacity - 1)) {
				newcapacity = capacity * 2;
				char* tmp = realloc(buffer, newcapacity);
				if(tmp == NULL) {
					syslog(LOG_ERR, "realloc");
					printf("Failed at realloc\n");
					free(buffer);
					fclose(datafile);
					exit(1);
				}
				capacity = newcapacity;
				buffer = tmp;
			}

			receivedbytes = recv(clsockfd, buffer + currentlength, 1, 0);
			if(receivedbytes < 0) {
				syslog(LOG_ERR, "recv");
				printf("Failed at recv\n");
				fclose(datafile);
				exit(1);
			}
			if(receivedbytes == 0) {
				if (currentlength > 0) {
        			fwrite(buffer, 1, currentlength, datafile);
        			fflush(datafile);
    			}
				break;
			}
			printf("Data received: %c\n", buffer[currentlength]);
			currentlength += receivedbytes;
			buffer[currentlength] = '\0';
			if (buffer[currentlength - 1] == '\n') {
        		fwrite(buffer, 1, currentlength, datafile);
				fflush(datafile);
				currentlength = 0;
    		}
		}
		fclose(datafile);
		datafile = fopen("/var/tmp/aesdsocketdata", "r");
		if (datafile == NULL) {
    		syslog(LOG_ERR, "fopen");
			printf("Failed at fopen 2\n");
		}
		char* line = NULL;
		size_t readlength = 0;
		ssize_t readbytes;
		syslog(LOG_INFO, "Sending data over network...");
		while((readbytes = getline(&line, &readlength, datafile)) != -1) {
			printf("Data read : %s\n", line);
			send(clsockfd, line, readbytes, 0);
		}
		free(line);
    	free(buffer);
		fclose(datafile);
		close(clsockfd);
		syslog(LOG_INFO, "Closed connection from %s", client_ip);
	}
	close(srvsockfd);
    remove("/var/tmp/aesdsocketdata");
    closelog();
	return 0;
}
