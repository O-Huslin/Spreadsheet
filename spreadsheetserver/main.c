/* 
 * File:   main.c
 * Author: Kathilee Ledgister
 *
 * Created on April 4, 2021, 11:53 AM
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
#include <inttypes.h>
#include <float.h>

/**
 * BE SURE TO COMPILE WITH AND LINK WITH :  "-pthread" link and compile option
 */

/*
 * Two user defined signals we use to communicate internally
 */
#define RECYCLE_TIMEOUT             10 // 10 seconds
#define SERVERHOSTNAME        NULL //"localhost"
#define SERVERPORT            "20020"        
#define CELL_TYPE_STRING    1
#define CELL_TYPE_FLOAT     2
#define CELL_TYPE_FORMULA   3
#define CELL_TYPE_INVALID   0
#define SHEET_COLUMNS       9
#define SHEET_ROWS          9
#define MAX_RECALCULATES    128
#define SHEET_BUF_SIZ       (MAX_RECALCULATES*SHEET_COLUMNS*SHEET_ROWS*2)

volatile int gbContinueProcessingSpreadSheet = 1;
pthread_t pthreadHandler;
int firstClientSocket = -1;

/*
 * One user defined signals we use to communicate internally
 */
#define SIGNAL_SEND_SPREADSHEET (SIGRTMIN+1)

/**
 * floward declare list
 */
#define STR_CELL_SIZE   128
typedef struct _tag_linkedlist clientlist_t;

typedef struct _tag_linkedlist {
    int clientSocket;

    pthread_t pthreadClient;
    clientlist_t *next;
} clientlist_t;

typedef struct _tag_cell {
    int type;
    char used;
    char updated;
    float fval;
    char sval[STR_CELL_SIZE];
} cell_t;

clientlist_t *listTop = NULL;
pthread_mutex_t gmutex_SSheet;
/**
 * package the last spread sheet here for sending
 */
char strLastSpreadSheet[SHEET_BUF_SIZ];
char sheetCopy[SHEET_BUF_SIZ];

cell_t sheet[SHEET_ROWS][SHEET_COLUMNS];

int numUsers = 0;

int isCellRef(char * cellref) {
    int nref = 0;
    do {
        if (cellref == NULL)
            break;

        if (tolower(cellref[0]) < 'a' || tolower(cellref[0]) > 'i')
            break;

        if (cellref[1] < '1' || cellref[1] > '9')
            break;

        nref = 1;
    } while (0);
    return nref;
}

/*
Gets the cell reference from formula 
 */
int getCellRefColumn(char * cellref) {
    int nref = -1;
    do {
        if (!isCellRef(cellref))
            break;

        nref = tolower(cellref[0]) - 'a';
    } while (0);
    return nref;
}

int getCellRefRow(char * cellref) {
    int nref = -1;
    do {
        if (!isCellRef(cellref))
            break;

        nref = cellref[1] - '1';
    } while (0);
    return nref;
}

int getFormulaType(char *formula) {
    int ftype = 0;
    do {
        if (formula == NULL)
            break;

        if (!strncasecmp(formula, "AVERAGE(", 8)) {
            ftype = 1;
            break;
        }
        if (!strncasecmp(formula, "RANGE(", 6)) {
            ftype = 2;
            break;
        }

        if (!strncasecmp(formula, "SUM(", 4)) {
            ftype = 3;
            break;
        }
    } while (0);
    return ftype;
}

int isFormula(char *formula) {
    int valid = 0;
    int ft;
    int offset = 0;
    char c1[3], r1[3];
    do {
        if (formula == NULL)
            break;

        if ((ft = getFormulaType(formula)) == 0)
            break;
        switch (ft) {
            case 1:
                offset = 8;
                break;
            case 2:
                offset = 6;
                break;
            case 3:
                offset = 4;
                break;
        }
        if (!isCellRef(&formula[offset]))
            break;

        c1[0] = formula[offset];
        r1[0] = formula[offset + 1];

        offset += 2;
        if (formula[offset] != ',')
            break;
        offset++;

        if (!isCellRef(&formula[offset]))
            break;

        c1[1] = formula[offset];
        r1[1] = formula[offset + 1];

        /**
         * either the row references must be the same
         * or the column references must be the same
         * we are processing only 1D elements
         */
        if (c1[0] != c1[1] && r1[0] != r1[1])
            break;

        offset += 2;
        if (formula[offset] != ')')
            break;

        offset++;
        if (formula[offset] != '\0')
            break;
        valid = 1;
    } while (0);
    return valid;
}

/*
 * If +123.457abc123 is in str then 'strtof' will make
 * endptr point to 'a' of abc. even though strtof will
 * return a valid float of +123.457 we do not consider this as a
 * valid float and we thus place it in the spreadsheet
 * as the string "+123.457abc123".
 */
int isnumstr(char *str) {
    int ret = 0;
    char *endptr;

    do {
        float fv = strtof(str, &endptr);

        if (endptr != NULL && endptr[0] != '\0')
            break;

        ret = 1;
    } while (0);

    return ret;
}

int getDataType(char *celldata) {
    int cType = CELL_TYPE_INVALID;
    do {
        if (celldata == NULL || celldata[0] == '\0')
            break;

        if (celldata[0] == '\'' && celldata[1] == '\0') {
            break;
        }

        if (isnumstr(celldata)) {
            cType = CELL_TYPE_FLOAT;
            break;
        }

        if (isFormula(celldata)) {
            cType = CELL_TYPE_FORMULA;
            break;
        }

        cType = CELL_TYPE_STRING;
    } while (0);
    return cType;
}

float fnAverage(int row, int col, char *r1, char *c1) {
    float sum = 0.0f;
    int count = 0;
    int r, c;

    for (int j = r1[0]; j <= r1[1]; j++)
        for (int i = c1[0]; i <= c1[1]; i++) {
            r = j - '1';
            c = i - 'a';
            sum += sheet[r][c].fval;
            count++;
            sheet[r][c].used = 1;
        }

    if (count > 0)
        sheet[row][col].fval = sum / (float) count;

    sheet[row][col].updated = 1;
    return sheet[row][col].fval;
}

float fnRange(int row, int col, char *r1, char *c1) {
    float rmin = FLT_MAX;
    float rmax = -FLT_MAX;
    int r, c;

    for (int j = r1[0]; j <= r1[1]; j++)
        for (int i = c1[0]; i <= c1[1]; i++) {
            r = j - '1';
            c = i - 'a';
            if (rmin > sheet[r][c].fval)
                rmin = sheet[r][c].fval;

            if (rmax < sheet[r][c].fval)
                rmax = sheet[r][c].fval;
            sheet[r][c].used = 1;
        }

    if (rmax == FLT_MIN)
        rmax = rmin; //SANITY CHECK

    sheet[row][col].fval = rmax - rmin;

    sheet[row][col].updated = 1;
    return sheet[row][col].fval;
}

float fnSum(int row, int col, char *r1, char *c1) {
    float sum = 0.0f;
    int r, c;

    for (int j = r1[0]; j <= r1[1]; j++)
        for (int i = c1[0]; i <= c1[1]; i++) {
            r = j - '1';
            c = i - 'a';
            sum += sheet[r][c].fval;
            sheet[r][c].used = 1;
        }

    sheet[row][col].fval = sum;

    sheet[row][col].updated = 1;
    return sheet[row][col].fval;
}

int evaluateCell(int col, int row) {
    int changed = 0;
    char *endptr;
    float fv;
    char c1[3], r1[3];
    int fType;

    switch (sheet[row][col].type) {
        case CELL_TYPE_STRING:
            //just keep the value
            sheet[row][col].used = 1;
            sheet[row][col].updated = 1;
            break;
        case CELL_TYPE_FLOAT:
            fv = sheet[row][col].fval;
            sheet[row][col].fval = strtof(sheet[row][col].sval, &endptr);
            if (fv != sheet[row][col].fval)
                changed = 1;
            sheet[row][col].used = 1;
            sheet[row][col].updated = 1;
            break;
        case CELL_TYPE_FORMULA:
            fType = getFormulaType(sheet[row][col].sval);
            fv = sheet[row][col].fval;
            switch (fType) {
                case 1://AVERAGE
                    c1[0] = tolower(sheet[row][col].sval[8]);
                    r1[0] = sheet[row][col].sval[9];
                    c1[1] = tolower(sheet[row][col].sval[11]);
                    r1[1] = sheet[row][col].sval[12];
                    fnAverage(row, col, r1, c1);
                    break;
                case 2://RANGE
                    c1[0] = tolower(sheet[row][col].sval[6]);
                    r1[0] = sheet[row][col].sval[7];
                    c1[1] = tolower(sheet[row][col].sval[9]);
                    r1[1] = sheet[row][col].sval[10];
                    fnRange(row, col, r1, c1);
                    break;
                case 3://SUM
                    c1[0] = tolower(sheet[row][col].sval[4]);
                    r1[0] = sheet[row][col].sval[5];
                    c1[1] = tolower(sheet[row][col].sval[7]);
                    r1[1] = sheet[row][col].sval[8];
                    fnSum(row, col, r1, c1);
                    break;
            }
            if (fv != sheet[row][col].fval)
                changed = 1;
            sheet[row][col].used = 1;
            sheet[row][col].updated = 1;
            break;
    }

    return changed;
}

int evaluateSheet(int mode) {
    int circ = 0;
    int changed = 0;
    int looper = 0;

    do {
        changed = 0;
        for (int j = 0; j < SHEET_ROWS; j++)
            for (int i = 0; i < SHEET_COLUMNS; i++)
                sheet[j][i].updated = sheet[j][i].used = 0;

        for (int j = 0; j < SHEET_ROWS; j++) {
            for (int i = 0; i < SHEET_COLUMNS; i++) {
                int chg = evaluateCell(i, j);
                if (chg != 0)
                    changed = chg;
                /*if (mode == 1
                        && sheet[j][i].updated == 1 && sheet[j][i].used == 1) {
                    circ = 1;
                    i = SHEET_COLUMNS + 1;
                    break;
                }*/
            }
        }
        looper++;
    } while (changed && !circ && looper < MAX_RECALCULATES);
    if (looper >= MAX_RECALCULATES)
        circ = 1; //we are calling this a circular reference
    return circ;
}

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

void notifyAllUsers(char *sheetCopy) {
    clientlist_t *list;
    int rc = 0;

    //-------------------------------------
    // TODO
    // notify all users in the linked list
    //-------------------------------------

    // for client in the linlked list
    for (list = listTop; list; list = list->next) {
        rc = sendall(list->clientSocket, sheetCopy, strlen(sheetCopy), 0);
        if (rc) {
            /**
             * for some reason all the data was not sent
             * Try resending the rest of the data
             * SANITY CHECK **************
             * CHECK: there could also be a bug that locks the program into this loop
             */
            while (rc) {
                rc = sendall(list->clientSocket, &sheetCopy[rc], strlen(&sheetCopy[rc]), 0);
            }
        }
    }
}

static void *sig_sendSheethandler() {
    sigset_t set;
    int s, sig;

    sigemptyset(&set);
    sigaddset(&set, SIGNAL_SEND_SPREADSHEET);
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
            if (sig == SIGNAL_SEND_SPREADSHEET) {
                do {
                    if (pthread_mutex_trylock(&gmutex_SSheet)) {
                        usleep(100000); // microseconds
                        // when you wake up
                        continue;
                    }
                    // make a copy of the sheet
                    strncpy(sheetCopy, strLastSpreadSheet, SHEET_BUF_SIZ - 1);
                    pthread_mutex_unlock(&gmutex_SSheet);
                    break;
                } while (1);

                notifyAllUsers(sheetCopy);
            }
            //s = pthread_sigmask(SIG_UNBLOCK, &set, NULL);
        }
    }
}

void updateSpreadSheet(char *data) {
    int cType;
    int offset = 0;

    do {


        if (pthread_mutex_trylock(&gmutex_SSheet)) {
            usleep(100000); // microseconds
            // when you wake up
            continue;
        }

        //-------------------------------------
        // update the spread sheet
        //check that the data is properly formatted
        //the third character must be an '=' sign
        if (data == NULL || strlen(data) < 4 || data[2] != '=')
            break;

        if ((cType = getDataType(&data[3])) == CELL_TYPE_INVALID)
            break;

        int column = getCellRefColumn(data);
        int row = getCellRefRow(data);

        if (column == -1 || row == -1)
            break;

        char savedCell[STR_CELL_SIZE];
        int savedType = sheet[row][column].type;
        strncpy(savedCell, sheet[row][column].sval, STR_CELL_SIZE);


        sheet[row][column].type = cType;
        strncpy(sheet[row][column].sval, &data[3], STR_CELL_SIZE - 1);
        sheet[row][column].sval[STR_CELL_SIZE - 1] = '\0'; //SANITY CHECK

        /**
         * if the evaluation requires more than "MAX_RECALCULATES"
         * interations then we are calling this acircular refernce. 
         * We will not put this formula in the spreadsheet
         */
        if (evaluateSheet((cType == CELL_TYPE_FORMULA ? 1 : 0))) {
            /**
             * We detect a circular reference so put back the original data
             */
            sheet[row][column].type = savedType;
            strncpy(sheet[row][column].sval, savedCell, STR_CELL_SIZE);
            evaluateSheet(0);
            break;
        }

        // package the last spread sheet in this variable
        // while no other thread can change it
        // strLastSpreadSheet == last spread sheet

        //The first element returned in the spreadsheet data ser
        //is the number of users
        
        offset += snprintf(&strLastSpreadSheet[offset],
                SHEET_BUF_SIZ - offset - 1, "%d\r\n", numUsers);
        /**
         * format must be: "1\r\n2\r\n3\r\n4\r\n/5\r\n6\r\n7\r\n8\r\n9\r\n\r\n"
         */
        for (int j = 0; j < SHEET_ROWS; j++)
            for (int i = 0; i < SHEET_COLUMNS; i++) {
                switch (sheet[j][i].type) {
                    case CELL_TYPE_FLOAT:
                    case CELL_TYPE_FORMULA:
                        offset += snprintf(&strLastSpreadSheet[offset],
                                SHEET_BUF_SIZ - offset - 1, "%f\r\n", sheet[j][i].fval);
                        break;
                    default:
                        offset += snprintf(&strLastSpreadSheet[offset],
                                SHEET_BUF_SIZ - offset - 1, "%s\r\n", sheet[j][i].sval);
                        break;
                }
            }
        strncpy(&strLastSpreadSheet[offset], "\r\n", SHEET_BUF_SIZ - offset - 1);
        strLastSpreadSheet[SHEET_BUF_SIZ - 1] = '\0'; //SANITY CHECK
        //-------------------------------------
        pthread_mutex_unlock(&gmutex_SSheet);

        /**
         * send the updated spreadsheet to all users in the linked list
         */
        // notify all users in the linked list
        union sigval sval;
        sval.sival_int = 0;
        int rc = pthread_sigqueue(pthreadHandler, SIGNAL_SEND_SPREADSHEET, sval);

        break;

    } while (1);
}

void * serverProcessor(void *arg) {
    int commSocket = -1;
    int ires;
    int total = 0; // how many bytes we've received
    fd_set readfds;
    fd_set exceptfds;
    struct timeval timeout;
    char recvbuf[4096];
    size_t recvbuflen = 4096;

    if (arg == NULL) {
        return NULL;
    }
    commSocket = *(int*) arg;
    free(arg);

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
                    printf("Socket Exception: Error reading scoket\n");
                } else if (FD_ISSET(commSocket, &readfds)) {
                    ires = recv(commSocket, &recvbuf[total], recvbuflen - total, 0);
                    /**
                     * process the data in the received buffer
                     */

                    if (ires == -1) {
                        switch (errno) {
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

                        total += ires;
                        recvbuf[total] = '\0';
                        char *pstr_find = strstr(recvbuf, "\r\n\r\n");
                        if (pstr_find) {
                            /**
                             * manage the buffer for the socket
                             */

                            char received_data[4096];
                            int ioffset = (int) (pstr_find - recvbuf);

                            strncpy(received_data, recvbuf, 4096);
                            received_data[ioffset] = '\0';
                            strncpy(recvbuf, &received_data[ioffset + 4], total - (ioffset + 4));
                            total = total - (ioffset + 4);
                            recvbuf[total] = '\0';

                            if (firstClientSocket == commSocket && !strcmp(received_data, "SHUTDOWN")) {
                                gbContinueProcessingSpreadSheet = 0; // stop processing
                                break;
                            } else {
                                /**
                                 * upon receipt of data - just update the spreadsheet
                                 */
                                updateSpreadSheet(received_data);
                            }
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
    struct timeval timeout;
    fd_set readfds;
    fd_set exceptfds;

    int bOptVal = 1;
    int bOptLen = sizeof (int);
    struct linger lingerOptVal;
    int lingerOptLen = sizeof (struct linger);

    /**
     * SANITY CHECK: Make sure the signal structure is clear
     */
    memset(sheet, 0x00, sizeof (cell_t) * SHEET_COLUMNS * SHEET_ROWS);
    for (int j = 0; j < SHEET_ROWS; j++)
        for (int i = 0; i < SHEET_COLUMNS; i++)
            sheet[j][i].type = CELL_TYPE_FLOAT;
    memset(&pthreadHandler, 0, sizeof (pthread_t));

    memset(&hints, 0x00, sizeof (struct addrinfo));
    hints.ai_family = AF_UNSPEC; //AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE; /* For wildcard IP address */

    do {
        /**
         * Resolve the server address and port 
         * change SERVERHOSTNAME and SERVERPORT to the values desired
         */

        rc = getaddrinfo(SERVERHOSTNAME, SERVERPORT, &hints, &result);
        if (rc != 0) {
            printf("ERROR: getaddrinfo failed with: %d\n", rc);
            printf("UNABLE to resolve the server PORT = [%s]\n", SERVERPORT);
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
            if ((rc = bind(commSocket, rp->ai_addr, rp->ai_addrlen)) == 0)
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
            printf("WEIRD ERROR: This should not occur. *** whats up ***\n");
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
         * we set linger so that is we crash the "**ALL**" of the last data will
         * still be sent
         */
        lingerOptVal.l_onoff = 0; // true - turn linger on
        lingerOptVal.l_linger = RECYCLE_TIMEOUT; // 10 seconds - wait before terminate
        bOptLen = setsockopt(commSocket, SOL_SOCKET, SO_KEEPALIVE, (char*) &bOptVal, bOptLen);
        bOptLen = setsockopt(commSocket, SOL_SOCKET, SO_LINGER, (char*) &lingerOptVal, lingerOptLen);

        /**
         * Create a thread to handle termination signals for network handler thread
         * We need a thread so that we can send signal to it - easily
         */
        if ((rc = pthread_create(&pthreadHandler, NULL, &sig_sendSheethandler, NULL))) {
            printf("Signal handler thread creation failed: [%d]\n", rc);
            close(commSocket);
            exit(EXIT_FAILURE);
        }

        pthread_mutex_init(&gmutex_SSheet, NULL);

        printf("Ready To Start Processing Spread Sheet Requests\n");

        do {
            // if the socket fail restart the socket
            rc = listen(commSocket, SOMAXCONN);
            if (rc == -1) {
                char *perr = strerror(errno);
                if (perr)
                    printf("Socket Listen error :  [%s]\n", perr);
                break;
            }
            //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
            //Process this socket to check for incomming requests
            do { // process the socket
                FD_ZERO(&readfds);
                FD_SET(commSocket, &readfds);

                FD_ZERO(&exceptfds);
                FD_SET(commSocket, &exceptfds);

                timeout.tv_sec = RECYCLE_TIMEOUT; // cycle every 10 seconds
                timeout.tv_usec = 0;

                rc = select(commSocket + 1, &readfds, NULL, &exceptfds, &timeout);
                switch (rc) {
                    case -1:
                        rc = 0;
                        switch (errno) {
                            case EINTR:
                                continue;
                                break;
                            default:
                                break;
                        }
                        break;

                    case 0: // timeout occurred
                        // the connection may be slow
                        rc = 0;
                        break; // just retry

                    default:
                        rc = 0;
                        if (FD_ISSET(commSocket, &exceptfds)) {
                            break;
                        } else if (FD_ISSET(commSocket, &readfds)) {
                            rc = 1;
                            break;
                        }
                        break;
                }
            } while (gbContinueProcessingSpreadSheet && !rc);

            //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

            if (gbContinueProcessingSpreadSheet) {
                // Accept a client socket
                int cSocket = accept(commSocket, NULL, NULL);
                if (cSocket == -1) {
                    switch (errno) {
                        case EINTR:
                        case EWOULDBLOCK:
                            /* just retry */
                            break;
                        default:
                            break;
                    }
                } else {
                    lingerOptVal.l_onoff = 0;
                    lingerOptVal.l_linger = RECYCLE_TIMEOUT; // 10 seconds
                    bOptLen = setsockopt(cSocket, SOL_SOCKET, SO_KEEPALIVE, (char*) &bOptVal, bOptLen);
                    bOptLen = setsockopt(cSocket, SOL_SOCKET, SO_LINGER, (char*) &lingerOptVal, lingerOptLen);


                    do {
                        int *pclientSocket = (int *) malloc(sizeof (int));
                        if (pclientSocket == NULL) {
                            printf("Error: NO mem processing client connection\n");
                            close(cSocket);
                            break;
                        }
                        // add socket to linked list
                        // add also the thread-id to the list structure
                        clientlist_t *plist = malloc(sizeof (clientlist_t));
                        if (plist == NULL) {
                            printf("Error: NO mem processing client connection\n");
                            close(cSocket);
                            free(pclientSocket);
                            break;
                        }
                        *pclientSocket = cSocket;

                        pthread_t thread;
                        if (pthread_create(&thread, NULL, serverProcessor, (void *) pclientSocket) != 0) {
                            printf("Error Setting up session for a client\n");
                            if (plist) {
                                free(plist);
                            }
                            close(cSocket);
                            free(pclientSocket);
                            break;
                        }

                        // add socket to linked list
                        // add also the thread-id to the list structure
                        plist->clientSocket = cSocket;
                        plist->next = NULL;
                        plist->pthreadClient = thread;
                        if (firstClientSocket == -1) {
                            firstClientSocket = cSocket;
                            /*this is the controller socket client*/
                        }
                        // DUMMY TEST LINKED LIST -- DO IT PROPERLY
                        plist->next = listTop;
                        listTop = plist;
                        numUsers++;
                        //-------------------------------------
                        // TODO
                        // add "plist" to the linked list
                        //-------------------------------------
                    } while (0);
                }
            }

        } while (gbContinueProcessingSpreadSheet);
    } while (0);

    if (commSocket != -1)
        close(commSocket);

    if (pthreadHandler != 0) {
        pthread_cancel(pthreadHandler);
        pthread_join(pthreadHandler, NULL);
    }

    // for client in the linlked list
    clientlist_t *list;
    for (list = listTop, listTop = NULL; list;) {
        // tell the other clients to shutdown
        // if they get a socket error - THEY SHOULD TERMINATE
        sendall(list->clientSocket, "SHUTDOWN\r\n\r\n", strlen("SHUTDOWN\r\n\r\n"), 0);
        clientlist_t *temp = list;
        list = list->next;
        if (temp->pthreadClient) {
            pthread_cancel(temp->pthreadClient);
            pthread_join(temp->pthreadClient, NULL);
        }
        close(temp->clientSocket);
        free(temp);
    }

    pthread_mutex_destroy(&gmutex_SSheet);

    return (EXIT_SUCCESS);
}

