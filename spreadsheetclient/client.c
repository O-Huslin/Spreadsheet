/* 
 * File:   main.c
 * Author: Kathilee Ledgister 620121618
 *         Okeno Christie 620096966
 *         Orley Huslin 620106728
 *
 * Created on April 4, 2021, 11:53 AM
 */

/**
 * IMPROVEMENTS
 * 1 Formulas use cells to calculate their values, even if the cell has another formula
 * 2 Spreadsheets can be saved to a file specified by the user
 * 3 Spreadsheets can be reloaded from a saved file
 * 4 All commands require '***' in front. eg. to shut down the spreadsheet enter "***SHUTDOWN"
 * 5 To save a file enter "***SAVE filename.extension"
 * 6 To load a file enter "***LOAD filename.extension"
 */

/* Extension from POSIX.1:2001. 
Structure to contain information about address of a service provider.  */
#define _POSIX_C_SOURCE 200112L // MUST be decleared
#define _GNU_SOURCE


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

/**
 * BE SURE TO COMPILE WITH AND LINK WITH :  "-pthread" link and compile option
 */

/*
 * Two user defined signals we use to communicate internally
 */
#define SIGNAL_QUIT_SPREADSHEET (SIGRTMIN+1)
#define RECYCLE_TIMEOUT             10 // 10 seconds
#define SERVERHOSTNAME        "localhost"
#define SERVERPORT            "20020"        
#define SHEET_COLUMNS       9
#define SHEET_ROWS          9
#define MAX_RECALCULATES    128
#define SHEET_BUF_SIZ       (MAX_RECALCULATES*SHEET_COLUMNS*SHEET_ROWS*2)
#define CELL_DISPLAY_SIZ    12
#define HORZ_CHAR           '-'
#define VERT_CHAR           '|'

volatile int gbContinueProcessingSpreadSheet = 1;
/**
 * we use this to terminate the connection on signal and 
 * "networkProcessor" won't cancel
 */
int gcommSocket = -1;

pthread_t pthreadClient;
pthread_t pthreadHandler;

/**
 * Handle termination notifications from the server.
 */
void handle_terminate() {
    do {
        gbContinueProcessingSpreadSheet = 0;
        if (pthreadClient != 0) {
            pthread_cancel(pthreadClient);
            pthread_join(pthreadClient, NULL);
            memset(&pthreadClient, 0, sizeof (pthread_t));
        }
        if (gcommSocket != -1)
            close(gcommSocket);
        gcommSocket = -1;
        exit(EXIT_SUCCESS);
    } while (0);
}

/**
 * Send all the input data to the server
 * @param socket
 * @param buf
 * @param len
 * @param flags
 * @return 
 */
int sendall(int socket, const char *buf, unsigned int len, int flags) {
    int total = 0; // how many bytes we've sent
    int bytesleft = len; // how many we have left to send
    int n, rc = -1;
    fd_set writefds;
    fd_set exceptfds;
    struct timeval timeout;

    while (total < len) {
        FD_ZERO(&writefds);
        FD_SET(socket, &writefds);

        FD_ZERO(&exceptfds);
        FD_SET(socket, &exceptfds);

        timeout.tv_sec = 5; //0; if we don't receive more data in 30 seconds terminate
        timeout.tv_usec = 0;

        n = select(socket + 1, NULL, &writefds, &exceptfds, &timeout);
        switch (n) {
            case -1:
                switch (errno) {
                    case EINTR:
                        break;
                    default:
                        //goto send_error;?
                        break;
                }
                break;

            case 0: // timeout occurred
                // the connection may be slow
                break;

            default:
                if (FD_ISSET(socket, &exceptfds)) {
                    //goto send_error;?
                } else if (FD_ISSET(socket, &writefds)) {
                    n = send(socket, buf + total, bytesleft, flags);
                    if (n == -1) {
                        switch (errno) {
                            case EWOULDBLOCK:
                                break;
                            default:
                                //goto send_error;?
                                break;
                        }
                    } else if (n > 0) {
                        total += n;
                        bytesleft -= n;
                    }
                }
                break;
        }
    }

send_error:
    rc = bytesleft < 0 ? 0 : bytesleft;
    return rc; // return bytesleft: should be (0) on success
}

/**
 * Draw the spreadsheet
 * @param spreadsheet_data- from the server
 */
void drawSpreadSheet(char *spreadsheet_data) {
    char *cpos, *nextpos;
    char cbuff[CELL_DISPLAY_SIZ + 1];
    int s;
    system("clear"); //Clear the terminal screen
    // draw the spread sheet now 
    cpos = nextpos = spreadsheet_data;

    nextpos = strstr(cpos, "\r\n");
    nextpos[0] = '\0';
    for (s = 0; cpos[s] && isspace(cpos[s]); s++);
    strncpy(cbuff, &cpos[s], CELL_DISPLAY_SIZ);
    cbuff[CELL_DISPLAY_SIZ] = '\0';
    nextpos[0] = '\r';

    printf("The number of spreadsheet users is: [%s]\n\n", cbuff);
    printf("   ");
    for (int j = 0; j < SHEET_COLUMNS; j++) {
        printf("      %c      ", 'A' + j);
    }
    printf("\n");

    // printf("  %c\n", HORZ_CHAR);
    for (int i = 0; i < SHEET_ROWS; i++) {
        printf("   ");
        for (int j = 0; j < SHEET_COLUMNS; j++) {
            printf("%c", HORZ_CHAR);
            for (int k = 0; k < CELL_DISPLAY_SIZ; k++) {
                printf("%c", HORZ_CHAR);
            }
        }
        printf("%c\n", HORZ_CHAR);

        printf("%2d ", i + 1);
        for (int j = 0; j < SHEET_COLUMNS; j++) {
            printf("%c", VERT_CHAR);
            nextpos += 2;
            cpos = nextpos;

            nextpos = strstr(cpos, "\r\n");
            if (nextpos != NULL)
                nextpos[0] = '\0';
            for (s = 0; cpos[s] && isspace(cpos[s]); s++);
            strncpy(cbuff, &cpos[s], CELL_DISPLAY_SIZ);
            for (int k = strlen(&cpos[s]); k < CELL_DISPLAY_SIZ; k++)
                cbuff[k] = ' ';
            cbuff[CELL_DISPLAY_SIZ] = '\0';
            if (nextpos != NULL)
                nextpos[0] = '\r';
            printf("%s", cbuff);
        }
        printf("%c\n", VERT_CHAR);
    }
    printf("   ");
    for (int j = 0; j < SHEET_COLUMNS; j++) {
        printf("%c", HORZ_CHAR);
        for (int k = 0; k < CELL_DISPLAY_SIZ; k++) {
            printf("%c", HORZ_CHAR);
        }
    }
    printf("%c\n\n", HORZ_CHAR);

    printf("Please enter a value for the spread sheet in one of the following formats \n");
    printf("\t\t\t1.] cell address = cell value\n");
    printf("\t\t\t1.] cell address = cell formula\n");
    printf("Formula ranges must be 1D rows or columns\n");
    printf("e.g. B3=average(A2,a6)\n");
    printf("Enter SpreadSheet Data : ");
    fflush(stdout);
}

/**
 * Handle signals to quit the spreadsheet
 * @param arg
 * @return 
 */
static void *sig_handler(void *arg) {
    sigset_t set;
    int s, sig;

    sigemptyset(&set);
    sigaddset(&set, SIGNAL_QUIT_SPREADSHEET);
    s = pthread_sigmask(SIG_BLOCK, &set, NULL);
    if (s != 0) {
        printf("Fatal Error starting spreadsheet handler [%d]", errno);
        exit(EXIT_FAILURE);
    }
    for (;;) {
        sig = 0; // SANITY
        s = sigwait(&set, &sig);
        if (s == 0) {
            //s = pthread_sigmask(SIG_BLOCK, &set, NULL);
            if (sig == SIGNAL_QUIT_SPREADSHEET) {
                handle_terminate();
            }
            //s = pthread_sigmask(SIG_UNBLOCK, &set, NULL);
        }
    }
}

/**
 * Thread to handles receiving communications from the server.
 * When the spreadsheet is received it displays the spreadsheet.
 * When the shutdown command is received, it sends the 
 * 'SIGNAL_QUIT_SPREADSHEET' signal that terminates the program
 * @param arg
 * @return 
 */
void * networkProcessor(void *arg) {
    int commSocket = *(int*) arg;
    int ires;
    int total = 0; // how many bytes we've received
    fd_set readfds;
    fd_set exceptfds;
    struct timeval timeout;
    char recvbuf[SHEET_BUF_SIZ];
    size_t recvbuflen = SHEET_BUF_SIZ;

    do {

        // process the socket
        FD_ZERO(&readfds);
        FD_SET(commSocket, &readfds);

        FD_ZERO(&exceptfds);
        FD_SET(commSocket, &exceptfds);

        memset(&timeout, 0x00, sizeof (struct timeval));
        timeout.tv_sec = RECYCLE_TIMEOUT; // cycle every 10 seconds
        timeout.tv_usec = 0;

        ires = select(commSocket + 1, &readfds, NULL, &exceptfds, &timeout);
        switch (ires) {
            case -1: // error occurred
                switch (errno) {
                    case ECONNRESET:
                        // terminate this client handler
                        //if the server shuts down the connection we terminate this client
                        gbContinueProcessingSpreadSheet = 0;
                        printf("Socket Connection TERMINATED by server\n");
                        union sigval sval;
                        sval.sival_int = 0;
                        pthread_sigqueue(pthreadHandler, SIGNAL_QUIT_SPREADSHEET, sval);

                        break;
                    case EBADF:
                        /**
                         * An invalid file descriptor was given in one of the sets.
                         * (Perhaps a file descriptor that was already closed, or one
                         * on which an error has occurred.)
                         * */
                    case ENOMEM:
                        /**
                         * Unable to allocate memory for internal tables.
                         * */
                    case EINTR:
                        /**
                         * A signal was caught
                         * */
                    default:
                        /**
                         * Just go back to listening for data from the server or the user
                         */
                        continue;
                        break;
                }
                break;

            case 0: // timeout occurred
                /**
                 * Just go back to listening for data from the server or the user
                 */
                continue;
                break;

            default:
                if (FD_ISSET(commSocket, &exceptfds)) {
                    printf("Socket Exception: Error reading socket\n");
                } else if (FD_ISSET(commSocket, &readfds)) {
                    ires = recv(commSocket, &recvbuf[total], recvbuflen - total, 0);
                    /**
                     * process the data in the received buffer
                     */

                    if (ires == -1) {
                        switch (errno) {
                            case ECONNRESET:
                                // terminate this client handler
                                gbContinueProcessingSpreadSheet = 0;
                                printf("Socket Connection TERMINATED by server\n");
                                union sigval sval;
                                sval.sival_int = 0;
                                pthread_sigqueue(pthreadHandler, SIGNAL_QUIT_SPREADSHEET, sval);

                                break;
                            case EWOULDBLOCK:
                                /**
                                 * reading would blcok the socket - what sould we do
                                 * -- default -- just wait for more data
                                 */
                                break;
                            default:
                                /**
                                 * There is some other error -- print it
                                 * but we still continue
                                 */
                            {
                                char *perr = strerror(errno);
                                if (perr)
                                    printf("Socket Read Error :  [%s]\n", perr);
                            }
                                break;
                        }
                    } else if (ires >= 0) {
                        // it could be 0: for zero length data
                        // store it in the large buffers
                        // "\r\n\r\n" length = 4

                        /**
                         * when we receive the "\r\n\r\n" we know that this is
                         * the end of a command from the server. Process the data and save
                         * the rest of the data that may be in the buffer, if any.
                         */
                        total += ires;
                        recvbuf[total] = '\0';
                        char *pstr_find = strstr(recvbuf, "\r\n\r\n");
                        if (pstr_find) {
                            /**
                             * When we find the \r\n\r\n at the end of the data set
                             * we know that we have gotten all the spreadsheet data
                             * update now
                             */
                            /**
                             * manage the buffer for the socket
                             */

                            char received_data[SHEET_BUF_SIZ];
                            int ioffset = (int) (pstr_find - recvbuf);

                            strncpy(received_data, recvbuf, SHEET_BUF_SIZ);
                            received_data[ioffset] = '\0';
                            strncpy(recvbuf, &received_data[ioffset + 4], total - (ioffset + 4));
                            total = total - (ioffset + 4);
                            recvbuf[total] = '\0';

                            /**
                             * if we receive a message from the server to end the program
                             * we send "SIGNAL_QUIT_SPREADSHEET" to the "pthreadHandler" thread
                             * ---------------------------------------------------------------
                             * */
                            // if server says terminate then
                            if (!strncmp(received_data, "SHUTDOWN", 8)) {
                                gbContinueProcessingSpreadSheet = 0;
                                if (pthreadHandler != 0) {
                                    union sigval sval;
                                    sval.sival_int = 0;
                                    pthread_sigqueue(pthreadHandler, SIGNAL_QUIT_SPREADSHEET, sval);
                                }
                                continue;
                            }

                            //-------------------------
                            /* OTHERWISE */
                            //-------------------------
                            /**
                             * upon receipt of data - just update the spreadsheet
                             */

                            drawSpreadSheet(received_data);
                        }
                    }
                }
                break;
        }
    } while (gbContinueProcessingSpreadSheet);

    return NULL;
}

int main(int argc, char** argv) {
    int rc;
    int commSocket = -1;
    struct addrinfo *rp;
    struct addrinfo *result = NULL;
    struct addrinfo hints;

    int bOptVal = 1;
    int bOptLen = sizeof (int);
    struct linger lingerOptVal;
    int lingerOptLen = sizeof (struct linger);

    char formula_data[1024];

    /**
     * SANITY CHECK: Make sure the signal structure is clear
     */
    memset(&pthreadClient, 0, sizeof (pthread_t));
    memset(&pthreadHandler, 0, sizeof (pthread_t));

    memset(&hints, 0x00, sizeof (struct addrinfo));
    hints.ai_family = AF_UNSPEC; //AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    do {
        /**
         * Resolve the server address and port 
         * change SERVERHOSTNAME and SERVERPORT to the values desired
         */

        rc = getaddrinfo(SERVERHOSTNAME, SERVERPORT, &hints, &result);
        if (rc != 0) {
            printf("ERROR: getaddrinfo failed with: %d\n", rc);
            printf("UNABLE to resolve the server %s\n", SERVERHOSTNAME);
            break;
        }

        /*
         * Create a SOCKET for connecting to server
         * getaddrinfo() returns a list of address structures.
         * we will try each address until we successfully connect.
         * If socket(...) fails, we close the socket and try the next address.
         *  
         */

        for (rp = result; rp != NULL; rp = rp->ai_next) {
            commSocket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (commSocket == -1)
                continue;

            /*
             *  make sure the socket is nonblocking 
             */
            //if ((rc = fcntl(commSocket, F_SETFL, O_NONBLOCK)) != -1) {
            if ((rc = connect(commSocket, rp->ai_addr, rp->ai_addrlen)) == 0)
                break; /* Success */
            //}

            close(commSocket);
            commSocket = -1;
        }

        // Setup the TCP listening socket INADDR_ANY
        if (rp == NULL) { /* No address succeeded */
            printf("ERROR: we could not connect to the server on any address\n");
            break;
        }

        /*
         * SANITY CHECK : this should not occur
         */
        if (commSocket == -1) { /* wah0 - horsey */
            printf("WEIRD ERROR: Thia should not occur. *** whats up ***\n");
            break;
        }

        /* No longer needed */
        if (result) {
            freeaddrinfo(result);
            result = NULL;
        }

        /**
         * set the keep alive so that the socket is always open
         * the connection is not closed due to inactivity
         * 
         * we set linger so that sockets receive 'ECONNRESET' upon termination
         * or if the server program crashes
         */

        lingerOptVal.l_onoff = 1; // true - turn linger on
        lingerOptVal.l_linger = 0; //RECYCLE_TIMEOUT; // 10 seconds - wait before terminate
        bOptLen = setsockopt(commSocket, SOL_SOCKET, SO_KEEPALIVE, (char*) &bOptVal, bOptLen);
        bOptLen = setsockopt(commSocket, SOL_SOCKET, SO_LINGER, (char*) &lingerOptVal, lingerOptLen);

        /**
         * Create a thread to handle termination signals for network handler thread
         * We need a thread so that we can send signal to it - easily
         */
        if ((rc = pthread_create(&pthreadHandler, NULL, &sig_handler, NULL))) {
            printf("Signal handler thread creation failed: [%d]\n", rc);
            exit(EXIT_FAILURE);
        }

        /**
         * Create client processing thread now to handle
         * network receiving and spread sheet update
         */
        if ((rc = pthread_create(&pthreadClient, NULL, &networkProcessor, (void *) &commSocket))) {
            printf("Client network handling thread creation failed: [%d]\n", rc);
            exit(EXIT_FAILURE);
        }

        gcommSocket = commSocket;

        //request th server to send us a refresh of the spreadsheet
        rc = sendall(commSocket, "***REFRESH\r\n\r\n", strlen("***REFRESH\r\n\r\n"), 0);
        printf("Ready To Start Processing Spread Sheet Requests\n");

        do {
            printf("Please enter a value for the spread sheet in one of the following formats \n");
            printf("\t\t\t1.] cell address = cell value\n");
            printf("\t\t\t1.] cell address = cell formula\n");
            printf("Formula ranges must be 1D rows or columns\n");
            printf("e.g. B3=average(A2,a6)\n");
            printf("Enter SpreadSheet Data : ");

            //Read all user inputs
            /* Get the cell data, with size limit. */
            fgets(formula_data, 1022, stdin);
            /* Remove trailing newline, if there. */
            if ((strlen(formula_data) > 0) && (formula_data[strlen(formula_data) - 1] == '\n'))
                formula_data[strlen(formula_data) - 1] = '\0';
            formula_data[1023] = '\0'; // SANITY

            /********************************************************************
             * PACKAGE the spread sheet data and send it to the server now
             * 
             * send it off to the server now terminate with "\r\n\r\n"
             *********************************************************************
             */
            // make sure to terminate with "\r\n\r\n"
            strncat(formula_data, "\r\n\r\n", 1023);

            rc = sendall(commSocket, formula_data, strlen(formula_data), 0);
            if (rc) {
                /**
                 * for some reason all the data was not sent
                 * Try resending the rest of the data
                 * SANITY CHECK **************
                 * CHECK: there could also be a bug that locks the program into this loop
                 */
                while (rc) {
                    rc = sendall(commSocket, &formula_data[rc], strlen(&formula_data[rc]), 0);
                }
            }
        } while (gbContinueProcessingSpreadSheet);

    } while (0);
    //Upon termination, try to clean up used resources
    if (commSocket != -1)
        close(commSocket);

    if (pthreadClient != 0)
        pthread_join(pthreadClient, NULL);
    if (pthreadHandler != 0)
        pthread_join(pthreadHandler, NULL);

    return (EXIT_SUCCESS);
}

