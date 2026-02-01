#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <errno.h>
#include <limits.h>

#define INPUT_FILENAME "input.txt"

#define MAX_GRID_SIZE_N 500
#define MAX_NUM_TRUCKS_D 250
#define MAX_SOLVERS_S MAX_NUM_TRUCKS_D
#define MAX_TURNS_T 500
#define MAX_TOLL_BOOTHS_B 500
#define MAX_PKGS_PER_TRUCK 20

#define MAX_TRUCKS MAX_NUM_TRUCKS_D
#define TRUCK_MAX_CAP MAX_PKGS_PER_TRUCK
#define MAX_NEW_REQUESTS 50
#define MAX_TOTAL_PACKAGES 5000

#define MSG_RECV_HELPER_MTYPE 2
#define MSG_SEND_HELPER_MTYPE 1
#define MSG_SEND_SOLVER_SET_TRUCK 2
#define MSG_SEND_SOLVER_GUESS_AUTH_STRING 3
#define MSG_RECV_SOLVER_MTYPE 4

#define UP 'u'
#define DOWN 'd'
#define RIGHT 'r'
#define LEFT 'l'
#define STAY 's'

char AUTH_STRING_LETTERS[] = {'u', 'd', 'r', 'l'};

typedef struct PackageRequest {
	int packageId;
	int pickup_x;
	int pickup_y;
	int dropoff_x;
	int dropoff_y;
	int arrival_turn;
	int expiry_turn;
} PackageRequest;

typedef struct MainSharedMemory {
	char authStrings[MAX_TRUCKS][TRUCK_MAX_CAP + 1];
	char truckMovementInstructions[MAX_TRUCKS];
	int pickUpCommands[MAX_TRUCKS];
	int dropOffCommands[MAX_TRUCKS];
	int truckPositions[MAX_TRUCKS][2];
	int truckPackageCount[MAX_TRUCKS];
	int truckTurnsInToll[MAX_TRUCKS];
	PackageRequest newPackageRequests[MAX_NEW_REQUESTS];
	int packageLocations[MAX_TOTAL_PACKAGES][2];
} MainSharedMemory;

typedef struct TurnChangeResponse {
	long mtype;
	int turnNumber;
	int newPackageRequestCount;
	int errorOccured;
	int finished;
} TurnChangeResponse;

typedef struct TurnReadyRequest {
	long mtype; 
} TurnReadyRequest;

typedef struct SolverRequest {
	long mtype;
	int truckNumber;
	char authStringGuess[TRUCK_MAX_CAP + 1];
} SolverRequest;

typedef struct SolverResponse {
	long mtype;
	int guessIsCorrect;
} SolverResponse;

typedef enum {
    PACKAGE_WAITING,
    PACKAGE_ON_TRUCK,
    PACKAGE_ASSIGNED,
    PACKAGE_DELIVERED,
    PACKAGE_EXPIRED
} PackageStatus;

typedef struct PackageInfo {
    PackageRequest request;
    PackageStatus status;
    int current_x;
    int current_y;
    int on_truck_id;
    int movedThisTurn;
    int has_expired;
} PackageInfo;

typedef struct TruckInfo {
	int current_x;
	int current_y;
	int package_count;
	int assigned_count;
	int packages_on_board[TRUCK_MAX_CAP];
	int packages_assigned[TRUCK_MAX_CAP];
	int turns_in_toll;
} TruckInfo;

int g_size_of_grid_N, g_num_trucks_D, g_num_solvers_S, g_turn_num_last_req_T, g_num_toll_booths_B;

key_t g_shm_key;
key_t g_main_msgq_key;
key_t g_solver_msg_keys[MAX_SOLVERS_S];
int g_solver_msgid[MAX_SOLVERS_S];
int g_helper_msgid;
char g_auth_string[TRUCK_MAX_CAP +1];
TurnChangeResponse g_TurnChangeResponse;
TurnReadyRequest g_TurnReadyRequest;
SolverRequest g_Solver_Request;
SolverResponse g_Solver_Response;
PackageInfo g_packageinfo[MAX_TOTAL_PACKAGES];
TruckInfo g_truckinfo[MAX_NUM_TRUCKS_D];

PackageRequest active_packages[MAX_TOTAL_PACKAGES];
int active_package_count = 0;

MainSharedMemory * g_mainShmPtr;

void read_input_file();
void create_shared_memory();
void create_helper_and_solver_msgqs();
void cleanup();
void guess_auth_string(int,int);
void print_turn_change_response_from_helper();
int recv_turn_change_response_from_helper();
void process_turn_change_response();
void send_turn_ready_request_to_helper();
void initialize_shared_memory();
void initialize_package_info();
void process_packages();
void initialize_truck_info();
char truck_movements(int);
void assign_truck(int);
void cleanup_shm_auth_strings_pickups_dropoffs();

int main()
{

	read_input_file();

	create_shared_memory();

	initialize_package_info();

	initialize_truck_info();

	initialize_shared_memory();

	create_helper_and_solver_msgqs();

	while (1) {

		if(recv_turn_change_response_from_helper() == 1) {
			break;
		}
		cleanup_shm_auth_strings_pickups_dropoffs();
		process_packages();
		
		for(int i = 0;i<g_num_trucks_D;i++){
			printf("the truck %d has %d no of packages in it as per shared memory in the beginning of the turn\n",i,g_mainShmPtr->truckPackageCount[i]);
			guess_auth_string(i,g_truckinfo[i].package_count);
			assign_truck(i);
			g_mainShmPtr->truckMovementInstructions[i] = truck_movements(i);
			printf("movement %c assigned to truck %d\n",g_mainShmPtr->truckMovementInstructions[i],i);
		}
	
		send_turn_ready_request_to_helper();
	}

	cleanup();

	return 0;
}

void read_input_file()
{
	FILE *fp;
	int lines_read, i;

	fp = fopen(INPUT_FILENAME, "r");

	if (fp == NULL) {
		perror ("fopen");
		exit(1);
	}

	lines_read = fscanf(fp, "%d\n %d\n %d\n %d\n %d\n %d\n %d", 
			&g_size_of_grid_N, 
			&g_num_trucks_D, &g_num_solvers_S, 
			&g_turn_num_last_req_T, &g_num_toll_booths_B, 
			&g_shm_key, &g_main_msgq_key);

	if (lines_read != 7) {
		printf("Error in input file %s\n", INPUT_FILENAME);
		exit(1);
	}

	printf("Input File Contents read:\n");
	printf("\tN=%d, D=%d, S=%d, T=%d, B=%d, shm_key=%d, main_msgq_key=%d\n", 
			g_size_of_grid_N, 
			g_num_trucks_D, g_num_solvers_S, 
			g_turn_num_last_req_T, g_num_toll_booths_B, 
			g_shm_key, g_main_msgq_key);

	lines_read = 0;

	for (i = 0; i < g_num_solvers_S; i++) {
		lines_read = fscanf(fp, "%d\n", (int *) g_solver_msg_keys+i);

		if(lines_read != 1) {
			printf("Error reading msgq for solver %d\n", i+1);
			exit(1);
		}

		printf("\tsolver_msgq_key[%d]=%d\n", i, g_solver_msg_keys[i]);
	}

	fclose(fp);
}

void create_shared_memory()
{
	int shmID;

	shmID = shmget(g_shm_key, sizeof(MainSharedMemory), 0666);
	if (shmID == -1) {
		perror("shmget");
		exit(1);
	}

	g_mainShmPtr = shmat(shmID, NULL, 0);
	if (g_mainShmPtr == (void *) -1) {
		perror("shmat");
		exit(1);
	}
}

void create_helper_and_solver_msgqs()
{
	int i;

	g_helper_msgid = msgget(g_main_msgq_key, 0644 | IPC_CREAT);
	if (g_helper_msgid == -1) {
		perror("msgget");
		exit(1);
	}

	for (i = 0; i < g_num_solvers_S; i++) {
		g_solver_msgid[i] = msgget(g_solver_msg_keys[i], 0644 | IPC_CREAT);
		if (g_solver_msgid[i] == -1) {
			perror("msgget");
			exit(1);
		}
	}
}

void cleanup()
{
	shmdt(g_mainShmPtr);
}

void guess_auth_string(int truck_num, int num_pkgs)
{
	int auth_string_size = sizeof(AUTH_STRING_LETTERS);
	int num_alpha = sizeof(AUTH_STRING_LETTERS)/sizeof(AUTH_STRING_LETTERS[0]);
	unsigned long total_combinations = 1;

	for (int i=0; i< num_pkgs; i++) {
		total_combinations = total_combinations * (unsigned long)num_alpha;
	}

	g_Solver_Request.mtype = MSG_SEND_SOLVER_SET_TRUCK;
	g_Solver_Request.truckNumber = truck_num;

	if(msgsnd(g_solver_msgid[0], &g_Solver_Request, 
		  sizeof(g_Solver_Request) - sizeof(long), 0) == -1) {
		perror("Error in first msgsnd to solver");
		exit(1);
	}
	for(unsigned long i = 0; i<total_combinations; i++){
		unsigned long temp = i;
		
		for(int j = 0; j<num_pkgs; ++j){
			g_auth_string[num_pkgs - j - 1] = 
			AUTH_STRING_LETTERS[temp% num_alpha];
			temp /= num_alpha;
		}
		
		g_auth_string[num_pkgs] = '\0';
		

		g_Solver_Request.mtype = MSG_SEND_SOLVER_GUESS_AUTH_STRING;
		strncpy(g_Solver_Request.authStringGuess, 
		    g_auth_string, sizeof(g_Solver_Request.authStringGuess));

printf("msgsnd to solver: truck_num=%d, auth_string=%s\n", truck_num, g_Solver_Request.authStringGuess);
		if(msgsnd(g_solver_msgid[0], &g_Solver_Request, 
			  sizeof(g_Solver_Request) - sizeof(long), 0) == -1) {
			perror("Error in second msgsnd to solver");
			exit(1);
		}

		if(msgrcv(g_solver_msgid[0], &g_Solver_Response, sizeof(g_Solver_Response) - sizeof(long), MSG_RECV_SOLVER_MTYPE, 0) == -1) {
			perror("Error in msgrcv from solver");
			exit(1);
		}

printf("guess_auth_string: Guessed auth string[%s] is %s for truck #%d\n", g_Solver_Request.authStringGuess, g_Solver_Response.guessIsCorrect == 0?"Incorrect":"Correct", g_Solver_Request.truckNumber);

		if(g_Solver_Response.guessIsCorrect == 1) {
			strncpy(g_mainShmPtr->authStrings[truck_num],g_auth_string,sizeof(g_mainShmPtr->authStrings[truck_num]));
			printf("placed -%s- auth string for %d\n",g_mainShmPtr->authStrings[truck_num],truck_num);
			break;
		}

	}

}

void print_turn_change_response_from_helper()
{
	printf("Response received from Helper function: \n");
	printf("\t mtype = %ld, turnNumber = %d, newPackageRequestCount = %d, errorOccured = %d, finished = %d\n", 
		g_TurnChangeResponse.mtype, g_TurnChangeResponse.turnNumber, 
		g_TurnChangeResponse.newPackageRequestCount, 
		g_TurnChangeResponse.errorOccured, 
		g_TurnChangeResponse.finished);
    
	int count = g_TurnChangeResponse.newPackageRequestCount;
	for (int i = 0; i < count && i < MAX_NEW_REQUESTS; i++) {
		PackageRequest *r = &g_mainShmPtr->newPackageRequests[i];
		int pid = r->packageId;
		int package_x, package_y;
		if (pid >= 0 && pid < MAX_TOTAL_PACKAGES) {
			package_x = g_mainShmPtr->packageLocations[pid][0];
			package_y = g_mainShmPtr->packageLocations[pid][1];
		}

		printf("pkgId=%d  pickup=(%d,%d)  drop=(%d,%d)  expiry=%d  location=(%d,%d)\n",
			pid, r->pickup_x, r->pickup_y, r->dropoff_x, r->dropoff_y,
			r->expiry_turn, package_x, package_y);
	}
}


int recv_turn_change_response_from_helper()
{
printf("msgrcv from helper: id=%d, size=%ld\n", g_helper_msgid, sizeof(g_TurnChangeResponse) - sizeof(long));
	if (msgrcv(g_helper_msgid, &g_TurnChangeResponse, 
		sizeof(g_TurnChangeResponse) - sizeof(long), 
			MSG_RECV_HELPER_MTYPE, 0) == -1) {
		perror("msgrcv from helper");
		exit(1);
	}
	print_turn_change_response_from_helper();

	if(g_TurnChangeResponse.errorOccured == 1) {
		printf("Helper response: errorOccured, exiting\n");
		exit(1);
	}
	if(g_TurnChangeResponse.finished == 1) {
		printf("Helper response: finished, exiting\n");
		return 1;
	}
	

	return 0;
}

void send_turn_ready_request_to_helper()
{
	g_TurnReadyRequest.mtype = MSG_SEND_HELPER_MTYPE;

printf("msgsnd to helper: id=%d, size=%ld\n", g_helper_msgid, sizeof(g_TurnReadyRequest) - sizeof(long));
	if (msgsnd(g_helper_msgid, &g_TurnReadyRequest, 
			sizeof(g_TurnReadyRequest) - sizeof(long), 0) == -1) {
		perror("msgsnd to helper");
		exit(1);
	}
}
int distance(int x1, int y1, int x2, int y2){
    int dist = abs(x1-x2) + abs(y1-y2);
    return dist;
}

void assign_truck(int turn){
    int D = g_num_trucks_D;

    for (int truck = 0; truck < D; ++truck) {
        int truck_x = g_mainShmPtr->truckPositions[truck][0];
        int truck_y = g_mainShmPtr->truckPositions[truck][1];

        int nearest_pid = -1;
        int min_dist = INT_MAX;

        if (g_truckinfo[truck].package_count == 1 || g_truckinfo[truck].assigned_count == 1) {                                        
            continue;
        }

        for (int k = 0; k < active_package_count; ++k) {
            int pid = active_packages[k].packageId;
            if (pid < 0 || pid >= MAX_TOTAL_PACKAGES) continue;

            PackageInfo *package = &g_packageinfo[pid];
            
            if (package->status != PACKAGE_WAITING) continue;              

            if (package->on_truck_id != -1) continue;

            int package_x = package->request.pickup_x;
            int package_y = package->request.pickup_y;
            int dist = distance(truck_x, truck_y, package_x, package_y);

            if (dist < min_dist) {
                min_dist = dist;
                nearest_pid = pid;
            }
        }

        if (nearest_pid != -1) {
			g_truckinfo[truck].packages_assigned[g_truckinfo[truck].assigned_count++] = nearest_pid; 
			g_packageinfo[nearest_pid].status = PACKAGE_ASSIGNED;

        } else {
            
        }
    }
}

void dropoff_package(int truck,int pid){
	printf("truck %d dropped %d package\n",truck,pid);
	g_mainShmPtr->dropOffCommands[truck] = pid;
	g_truckinfo[truck].package_count--;
	g_truckinfo[truck].packages_on_board[0] =-1;
	g_packageinfo[pid].status = PACKAGE_DELIVERED;
}
void pickup_package(int truck,int pid){
	printf("truck %d picked %d package\n",truck,pid);
	g_mainShmPtr->pickUpCommands[truck] = pid;
	g_truckinfo[truck].package_count++;
	g_truckinfo[truck].assigned_count--;
	g_truckinfo[truck].packages_on_board[0] = g_truckinfo[truck].packages_assigned[0];
	g_truckinfo[truck].packages_assigned[0] = -1;
	g_packageinfo[pid].status = PACKAGE_ON_TRUCK;
	g_packageinfo[pid].on_truck_id = truck;
}
char truck_movements(int truck){
    int truck_x= g_mainShmPtr->truckPositions[truck][0];
    int truck_y= g_mainShmPtr->truckPositions[truck][1];
	int package_id = -1;
    if(g_truckinfo[truck].package_count == 1)package_id = g_truckinfo[truck].packages_on_board[0];
	else if(g_truckinfo[truck].assigned_count == 1) package_id = g_truckinfo[truck].packages_assigned[0];
    if(package_id == -1){
        return 's';
    }
    int package_x, package_y;
    for(int i=0;i<MAX_TOTAL_PACKAGES;i++){
        if(active_packages[i].packageId == package_id){ 
            if(g_truckinfo[truck].package_count == 1){
				package_x = active_packages[i].dropoff_x;             
				package_y = active_packages[i].dropoff_y;
				break;
			}
			else{		
				package_x = active_packages[i].pickup_x;				
				package_y = active_packages[i].pickup_y;
				break;
			}
        }
    }
	int dis = distance(truck_x,truck_y,package_x,package_y);

	if(dis == 0){
		if(g_truckinfo[truck].package_count == 1){
			dropoff_package(truck,package_id);
		}else{
			pickup_package(truck,package_id);
			package_x = active_packages[package_id].dropoff_x;             
			package_y = active_packages[package_id].dropoff_y;
		}
	}
    if(truck_x < package_x){
        printf("truck moved r\n");
        return 'r';
    }else if(truck_x > package_x){
        printf("truck moved l\n");
        return 'l';
    }else if(truck_y < package_y){
        printf("truck moved d\n");
        return 'd';
    }else if(truck_y > package_y){
        printf("truck moved u\n");
        return 'u';
    }else{
        printf("truck did not move\n");
        return 's';
    }

}

void initialize_package_info()
{
	int i;

	for (i=0; i<MAX_TOTAL_PACKAGES; i++) {
		g_packageinfo[i].request.packageId = -1;
		g_packageinfo[i].status = PACKAGE_WAITING;
		g_packageinfo[i].current_x = -1;
		g_packageinfo[i].current_y = -1;
		g_packageinfo[i].on_truck_id = -1;
		g_packageinfo[i].movedThisTurn = 0;
		g_packageinfo[i].has_expired = 0;
	}
    active_package_count = 0;
}

void process_packages(){
    int count = g_TurnChangeResponse.newPackageRequestCount;
    for(int i=0;i<count;i++){
        PackageRequest newpackage = g_mainShmPtr->newPackageRequests[i];
        int pid = newpackage.packageId;
        
           g_packageinfo[pid].status = PACKAGE_WAITING;
           g_packageinfo[pid].request = newpackage;
           g_packageinfo[pid].on_truck_id = -1;
           g_packageinfo[pid].movedThisTurn = 0;
           g_packageinfo[pid].has_expired = 0;

           int is_active = 0;

            if(!is_active){
                active_packages[active_package_count++] = newpackage;
         }   

        }
}

void initialize_truck_info()
{
	int i, j;

	for (i=0; i<g_num_trucks_D; i++) {
		g_truckinfo[i].current_x=0;
		g_truckinfo[i].current_y=0;
		g_truckinfo[i].package_count=0;
		g_truckinfo[i].assigned_count=0;
		g_truckinfo[i].turns_in_toll=0;

		for(j = 0; j < TRUCK_MAX_CAP; j++) {
			g_truckinfo[i].packages_on_board[j] = -1;
			g_truckinfo[i].packages_assigned[j] = -1;
		}
	}
}

void initialize_shared_memory()
{
	int i;

	for (i=0; i< MAX_TRUCKS; i++) {
		strncpy(g_mainShmPtr->authStrings[i], "", sizeof(g_mainShmPtr->authStrings[i]));
		g_mainShmPtr->truckMovementInstructions[i] = 's';
		g_mainShmPtr->pickUpCommands[i] = -1;
		g_mainShmPtr->dropOffCommands[i] = -1;
	}
}

void cleanup_shm_auth_strings_pickups_dropoffs(){
    for (int i=0; i< MAX_TRUCKS; i++) {
        strcpy(g_mainShmPtr->authStrings[i], "");
        g_mainShmPtr->pickUpCommands[i] = -1;
        g_mainShmPtr->dropOffCommands[i] = -1;
    }
}
