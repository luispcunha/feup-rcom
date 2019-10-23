#include "linklayer.h"
#include "ll_consts.h"
#include "receiver_state_machine.h"
#include "transmitter_state_machine.h"

static int trasmitter_open(int fd);
static int receiver_open(int fd);
static int serial_port_setup(int port);

static struct termios oldtio;
volatile int STOP = false;
int flag = 1, conta = 1;

int sequence_number = 0;

static void alarm_handler(int signo) {
  printf("alarme # %d\n", conta);
	flag = 1;
	conta++;
}

int sendUA(int fd)
{

  uint8_t response[SU_FRAME_SIZE];
  response[0] = FLAG;
  response[1] = ADDR_RECEIV_RES;
  response[2] = CONTROL_UA;
  response[3] = response[1] ^ response[2];
  response[4] = FLAG;

  int res = write(fd, response, SU_FRAME_SIZE);
  log_debug("RECEIVER: UA sent to trasmitter(%x %x %x %x %x) (%d bytes written)\n",response[0],response[1],response[2],response[3],response[4], res);

  return 0;
}

int sendAck(int fd, bool type, int sequence_no)
{

  uint8_t sequence_byte = sequence_no ? 0x80 : 0x00;
  uint8_t control_byte = type ? (CONTROL_RR_BASE | sequence_byte) : (CONTROL_REJ_BASE | sequence_byte);
  uint8_t response[SU_FRAME_SIZE];

  response[0] = FLAG;
  response[1] = ADDR_RECEIV_RES;
  response[2] = control_byte;
  response[3] = response[1] ^ response[2];
  response[4] = FLAG;

  int res = write(fd, response, SU_FRAME_SIZE);

  if (type)
    log_debug("RECEIVER: ACK(%d) sent to trasmitter(%x %x %x %x %x) (%d bytes written)\n", sequence_no,response[0],response[1],response[2],response[3],response[4], res);
  else
    log_debug("RECEIVER: NACK(%d) sent to trasmitter(%x %x %x %x %x) (%d bytes written)\n", sequence_no,response[0],response[1],response[2],response[3],response[4], res);

    return 0;
}

bool valid_data_bcc(uint8_t frame[], size_t frame_size)
{

  uint8_t bcc_value = frame[frame_size - 2];
  uint8_t calculated_value = frame[4];

  for (int i = 5; i < frame_size - 2; i++)
  {

    calculated_value ^= frame[i];
  }

  return bcc_value == calculated_value;
}


typedef struct linklayer {

  char port[20];
  int baudRate;
  unsigned int sequenceNumber;
  unsigned int timeout;
  unsigned int numTransmissions;
  bool connectionEstablished;

  uint8_t frame[2];
}linklayer_info;

linklayer_info connection_info; 

int llopen(int port, int flag) {
  int fd = serial_port_setup(port);

  switch (flag) {
    case TRANSMITTER:
      return trasmitter_open(fd);
    case RECEIVER:
      return receiver_open(fd);
    default:
      return -1;
  }
}

int llclose(int fd) {
  sleep(1);

  if ( tcsetattr(fd,TCSANOW,&oldtio) == -1) {
    perror("tcsetattr");
    exit(-1);
  }

  return close(fd);
}

int serial_port_setup(int port) {
  char port_path[PORT_PATH_LENGTH];
  sprintf(port_path, "/dev/ttyS%d", port);

  printf("portpath: %s\n", port_path);

  int fd = open(port_path, O_RDWR | O_NOCTTY );
  if (fd < 0) {
    perror(port_path);
    return -1;
  }

  struct termios newtio;

  if (tcgetattr(fd, &oldtio) == -1) { /* save current port settings */
    perror("tcgetattr");
    return -1;
  }

  bzero(&newtio, sizeof(newtio));
  newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
  newtio.c_iflag = IGNPAR;
  newtio.c_oflag = 0;

  /* set input mode (non-canonical, no echo,...) */
  newtio.c_lflag = 0;

  newtio.c_cc[VTIME] = 20;   /* inter-character timer unused */
  newtio.c_cc[VMIN] = 1;   /* blocking read until 5 chars received */

  tcflush(fd, TCIOFLUSH);

  if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
    perror("tcsetattr");
    return -1;
  }

  printf("New termios structure set\n");

  return fd;
}

int trasmitter_open(int fd) {
  printf("-Establishing connection...\n");

  // Frame building
  uint8_t frame_set[SU_FRAME_SIZE];
  frame_set[0] = FLAG;
  frame_set[1] = ADDR_TRANSM_COMMAND;
  frame_set[2] = CONTROL_SET;
  frame_set[3] = frame_set[1] ^ frame_set[2];
  frame_set[4] = FLAG;

  //Alarm setup
  struct sigaction alarm_action;
  alarm_action.sa_handler = alarm_handler;
  sigaction(SIGALRM, &alarm_action, NULL);

  int res;

  // State-machine setup
  struct transmitter_state_machine st_machine;

  while (conta < 4) {
     if (flag) {
        st_machine.currentState = T_STATE_START;
        res = write(fd, frame_set, SU_FRAME_SIZE);
        printf("-SET message sent to Receiver(%d bytes written)\n", res);
        alarm(3);                 // activates 3 sec alarm
        flag=0;
        tcflush(fd, TCIOFLUSH);

        // wait for answer
        while (! flag && STOP==false) {

          uint8_t currentByte;
          res = read(fd,&currentByte,1);                              // returns after a char has been read or after timer expired
          printf("-Byte received from Receiver(0x%x)\n", currentByte);
          tsm_process_input(&st_machine,currentByte);                   // state-machine processes the read byte

          if (st_machine.currentState == T_STATE_STOP) {
              STOP=true;
              alarm(0);
              return fd;
          }
        }
     }
  }

  return -1;
}

int receiver_open(int fd) {

  int res;

  struct su_frame_rcv_state_machine st_machine;
  st_machine.currentState = R_STATE_START;
  st_machine.connectionEstablished = false;
  st_machine.currentByte_idx = 0;
  connection_info.connectionEstablished = false;
  connection_info.sequenceNumber = 0;

  while(true){
    uint8_t currentByte;
    res = read(fd, &currentByte, 1); /* returns after a char has been read or after timer expired */

    log_debug("RECEIVER: received byte(0x%x - char:%c)(read %d bytes)", currentByte,(char)currentByte,res);
    sm_processInput(&st_machine, currentByte); /* state-machine processes the read byte */

    if(st_machine.currentState == R_STATE_SU_STOP){
      sendUA(fd); /* Send unnumbered acknowledgement to sender */
      connection_info.connectionEstablished = true;
      return fd;
    }
  }

  return 0;
}

int write_data(int fd, char *buffer, int length)
{

  int res = 0;

  char frame[4];
  frame[0] = FLAG;
  frame[1] = ADDR_TRANSM_COMMAND;
  frame[2] = sequence_number ? 0x40 : 0x00;
  frame[3] = frame[1] ^ frame[2];

  uint8_t bcc2 = 0x00;

  res += write(fd, frame, 4);

  for (int i = 0; i < length; i++)
  {

    bcc2 ^= buffer[i];

    if (buffer[i] == FLAG || buffer[i] == ESC)
    {
      frame[0] = ESC;
      frame[1] = buffer[i] ^ ESC_XOR;
      res += write(fd, frame, 2);
    } else {
      res += write(fd, &buffer[i], 1);
    }
  }

  if (bcc2 == FLAG || bcc2 == ESC) {
      frame[0] = ESC;
      frame[1] = bcc2 ^ ESC_XOR;
      res += write(fd, frame, 2);
  } else {
    res += write(fd, &bcc2, 1);
  }
  
  frame[0] = FLAG;
  res += write(fd, frame, 1);

  printf("- Message sent to Receiver(%d bytes written)\n", res);

  return 0;
}

int llwrite(int fd, char * buffer, int length) {

  int res;
  // State-machine setup
  struct transmitter_state_machine st_machine;
	conta = 1;
	flag = 1;

  while (conta < 4) {

     if (flag) {
        st_machine.currentState = T_STATE_START;
        write_data(fd, buffer, length);
        alarm(3);                 // activates 3 sec alarm
        flag = 0;
        STOP = false;
        tcflush(fd, TCIOFLUSH);

        // wait for answer
        while (!flag && STOP == false) {

          printf("entrou no while\n");

          uint8_t currentByte;
          res = read(fd, &currentByte, 1);                              // returns after a char has been read or after timer expired
          printf("-Byte received from Receiver(0x%x)\n", currentByte);
          tsm_process_input(&st_machine, currentByte);                   // state-machine processes the read byte

          if (st_machine.currentState == T_STATE_STOP) {

              printf("frame 2: %x\n", st_machine.frame[2]);
              printf("test with %x\n", ((sequence_number  + 1)<< 7) | CONTROL_RR_BASE);

              //TODO: do this
              if (st_machine.frame[2] == ((sequence_number + 1) << 7) | CONTROL_RR_BASE) {
                printf("entrou no if\n");
                alarm(0);
                sequence_number = (sequence_number + 1) % 2;
                return res;
              }
          }
        }
     }
  }
  return res;
}

int llread(int fd, char* buffer){

  int res;

  struct su_frame_rcv_state_machine st_machine;
  st_machine.currentState = R_STATE_START;
  st_machine.connectionEstablished = connection_info.connectionEstablished;
  st_machine.currentByte_idx = 0;

  log_debug("RECEIVER: started listening for data");
  while (true)
  { /* loop for input */

    
    uint8_t currentByte;
    res = read(fd, &currentByte, 1); /* returns after a char has been read or after timer expired */
    
    log_debug("RECEIVER: received byte(0x%x - char:%c)(read %d bytes)", currentByte,(char)currentByte,res);
    sm_processInput(&st_machine, currentByte); /* state-machine processes the read byte */

    if (st_machine.currentState == R_STATE_SU_STOP)
    {
      sendUA(fd); /* Send unnumbered acknowledgement to sender */
      connection_info.sequenceNumber = 0;
    }
    else if (st_machine.currentState == R_STATE_I_STOP)
    {
      bool isDataBCCValid = valid_data_bcc(st_machine.frame, st_machine.currentByte_idx + 1);

      if (isDataBCCValid)
      { // if the data bcc is valid check for the sequence number

        log_debug("RECEIVER: Data BCC is valid");
        int packetSeqNumber = st_machine.frame[CTRL_INDEX] >> 6;

        log_debug("RECEIVER: Received frame with seq=%d, desired was seq=%d", packetSeqNumber, connection_info.sequenceNumber);
        if (packetSeqNumber == connection_info.sequenceNumber)
        { // correct sequence number, send ack with next seq number

          int nextSeqNumber = connection_info.sequenceNumber ? 0 : 1;
          sendAck(fd, true, nextSeqNumber);
          connection_info.sequenceNumber = nextSeqNumber;

          // copy read frame to frame storage
          for (int i = 4; i < st_machine.currentByte_idx - 2; i++)
            buffer[i - 4] = st_machine.frame[i];

          return st_machine.currentByte_idx - 6;
        }
        else
        { // duplicate frame, discard and send ack
          log_debug("RECEIVER: duplicate frame received");
          sendAck(fd, true, connection_info.sequenceNumber);
        }
      }
      else
      { // if the data bcc is invalid send nack asking for the same frame
        log_debug("RECEIVER: Data BCC is invalid");
        sendAck(fd, false, connection_info.sequenceNumber);
      }
    }
  }
}