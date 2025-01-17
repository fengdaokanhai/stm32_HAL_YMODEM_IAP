#include "kymodem.h"
#include "usart.h"
#include "drv_com.h"
#include "string.h"


ModemFrameTypedef g_frame;
ModemFrameStateTypedef  g_frame_state;
ModemPacketTypedef g_modem_rx_packet;
Ymodem_TypeDef g_ymodem;
ModemPacketTypedef g_modem_tx_packet;

static uint8_t g_tx_buff[2048];

static void Int2Str(uint8_t *p_str, uint32_t intnum);
static unsigned long str_to_u32(char* str);
static unsigned short crc16(const unsigned char *buf, unsigned long count);
static void crc16_reset(void);
static void crc16_sum(uint8_t data);
static char ymodem_parser_head(uint8_t  *buf, uint32_t sz ) ;
static void parser_frame_reset(void);
static char parser_frame(uint8_t data);



static char ymodem_tx_packet(uint8_t data);
void ymodem_tx_head_packet(uint8_t *p_data, const uint8_t *p_file_name,uint32_t file_name_length, uint32_t length,uint32_t  packet_size);
static uint32_t ymodem_tx_data_packet(uint8_t **p_source, uint8_t *p_packet, uint8_t pkt_nr, uint32_t size_blk, uint8_t sent_mode);
static void ymodem_tx_end_packet(uint8_t *p_data);



// 初始化
void ymodem_init(Ymodem_TypeDef *ymodem)
{
    g_ymodem = *ymodem;
}
// 复位
static void ymodem_reset()
{
    memset(&g_frame,0,sizeof(g_frame));
}

__IO char g_write_C_disable = 0;
// 接受
void ymodem_rx_handle(uint8_t *data,uint32_t rx_size)
{
    int error_code = 0;
    g_write_C_disable = 1;
    for(int i =0; i<rx_size; i++) {

        //解析数据包成功
        if(parser_frame(data[i])) {
            //printf("jiexi:%d",g_frame.head);
            g_modem_rx_packet.now_packet_index = g_frame.index;
            // 如果重复的index,就是上位机中止了
            if(g_modem_rx_packet.now_packet_index == g_modem_rx_packet.last_packet_index && g_modem_rx_packet.last_packet_index!=0 && g_frame.head!=EOT)
            {
                error_code = FRAME_PARSER_ABORT_ERROR;

                g_ymodem.ymodem_rx_finish_handle(PACKET_RX_ABORT_ERROR);
                goto error_exit;
            }

            // 已经接受到了数据
            if(g_modem_rx_packet.packet_state == PACKET_RX_WAIT)
            {
                g_modem_rx_packet.packet_state = PACKET_RX_FIND_HEAD;
            }
            switch(g_modem_rx_packet.packet_state)
            {
            case PACKET_RX_WAIT:

                break;
            case PACKET_RX_FIND_HEAD:
                // ack
                // find head
                if((g_frame.head == SOH || g_frame.head == STX)&& g_frame.index == 0) {
                    // find file name
                    if(ymodem_parser_head(g_frame.data,g_frame_state.frame_data_len) <0) {
                        error_code = FRAME_PARSER_USER_HAND_HEAD_ERROR;
                        goto error_exit;
                    }
                    if(g_ymodem.ymodem_rx_head_handle(g_modem_rx_packet.packet_file.name_data,g_modem_rx_packet.packet_file.name_data_size,g_modem_rx_packet.packet_file.file_size) !=0) {
                        error_code = FRAME_PARSER_USER_HAND_HEAD_ERROR;
                        goto error_exit;
                    }
                    g_modem_rx_packet.packet_state = PACKET_RX_FIND_DATA;
                    g_write_C_disable = 0;

                } else {

                    error_code = FRAME_PARSER_HEAD_NOT_HAND;
                    goto error_exit;
                }
                drv_com1_write(ACK);
                break;
            case PACKET_RX_FIND_DATA:


                // 如果发送了EOT,代表数据传输完成
                if(g_frame.head == EOT) {
                    // 先发送NAK
                    drv_com1_write(NAK);
                    g_modem_rx_packet.packet_state = PACKET_RX_FIND_END;
                    // 数据可能用SOH 128或者STX 1024传输
                } else if(g_frame.head ==SOH || g_frame.head==STX) {
                    drv_com1_write(ACK);
                    uint32_t offset =g_modem_rx_packet.packet_file.file_size - g_modem_rx_packet.packet_file.sent_rec_file_size;
                    uint32_t data_size = 0;
                    if(g_frame.head ==SOH) {
                        data_size = SOH_PACKET_SIZE;
                    } else {
                        data_size = STX_PACKET_SIZE;
                    }

                    if(offset<data_size) {
                        g_modem_rx_packet.packet_file.sent_rec_file_size += offset;
                        g_modem_rx_packet.packet_file.percent = g_modem_rx_packet.packet_file.sent_rec_file_size*100/g_modem_rx_packet.packet_file.file_size;
                        g_ymodem.ymodem_rx_data_handle(g_frame.data,offset,g_modem_rx_packet.packet_file.sent_rec_file_size,g_modem_rx_packet.packet_file.percent);
                    } else {
                        g_modem_rx_packet.packet_file.sent_rec_file_size += data_size;
                        g_modem_rx_packet.packet_file.percent = g_modem_rx_packet.packet_file.sent_rec_file_size*100/g_modem_rx_packet.packet_file.file_size;
                        g_ymodem.ymodem_rx_data_handle(g_frame.data,data_size,g_modem_rx_packet.packet_file.sent_rec_file_size,g_modem_rx_packet.packet_file.percent);
                    }
                } else {
                    error_code = FRAME_PARSER_DATA_REC_ERROR;
                    goto error_exit;

                }

                break;
            case PACKET_RX_FIND_END:
                drv_com1_write(ACK);
                // 如果是EOT,要发送"C"
                if(g_frame.head == EOT) {
                    g_write_C_disable = 0;
                    //g_write_C_disable = 0;
                    // 如果发送了SOH,查看里面有没有文件,没有文件代表传输结束
                } else if(g_frame.head == SOH) {

                    if(g_modem_rx_packet.packet_file.file_size == g_modem_rx_packet.packet_file.sent_rec_file_size) {
                        g_ymodem.ymodem_rx_finish_handle(PACKET_RX_FINISH_OK);
                    } else {
                        error_code =FRAME_PARSER_DATA_SIZE_NOTEQ_ERROR;
                        g_ymodem.ymodem_rx_finish_handle(PACKET_RX_DATA_SIZE_NOTEQ_ERROR);
                        goto error_exit;
                    }
                } else {
                    error_code = FRAME_PARSER_END_REC_ERROR;
                    goto error_exit;
                }
                break;
            default:
                error_code = FRAME_PARSER_HEAD_NOT_FIND;
                goto error_exit;

            }

            g_modem_rx_packet.last_packet_index = g_modem_rx_packet.now_packet_index;
        }
    }


    return;
error_exit:
    ymodem_reset();
    //g_write_C_disable=0;
    g_modem_rx_packet.packet_state = PACKET_RX_WAIT;
    g_ymodem.ymodem_rx_error_handle(error_code);
}


int i = 0;
// 接受时间处理
void ymodem_rx_time_handle(void)
{
    if(i++ <1000000) {
    } else {
        i = 0;
        if(g_write_C_disable==0) {
            drv_com1_write(CNC);
        }
    }
}


// int转字符串
static void Int2Str(uint8_t *p_str, uint32_t intnum)
{
    uint32_t i, divider = 1000000000, pos = 0, status = 0;

    for (i = 0; i < 10; i++)
    {
        p_str[pos++] = (intnum / divider) + 48;

        intnum = intnum % divider;
        divider /= 10;
        if ((p_str[pos-1] == '0') & (status == 0))
        {
            pos = 0;
        }
        else
        {
            status++;
        }
    }
}


// 字符串转str
static unsigned long str_to_u32(char* str)
{
    const char *s = str;
    unsigned long acc;
    int c;

    /* strip leading spaces if any */
    do {
        c = *s++;
    } while (c == ' ');

    for (acc = 0; (c >= '0') && (c <= '9'); c = *s++) {
        c -= '0';
        acc *= 10;
        acc += c;
    }
    return acc;
}

// crc 16
static unsigned short crc16(const unsigned char *buf, unsigned long count)
{
    unsigned short crc = 0;
    int i;

    while(count--) {
        crc = crc ^ *buf++ << 8;

        for (i=0; i<8; i++) {
            if (crc & 0x8000) {
                crc = crc << 1 ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

// crc reset
static void crc16_reset(void)
{
    g_frame_state.frame_crc=0;
}
// crc sum
static void crc16_sum(uint8_t data)
{
    int i=0;
    g_frame_state.frame_crc = g_frame_state.frame_crc ^ data++ << 8;

    for (i=0; i<8; i++) {
        if (g_frame_state.frame_crc & 0x8000) {
            g_frame_state.frame_crc =g_frame_state.frame_crc << 1 ^ 0x1021;
        } else {
            g_frame_state.frame_crc =g_frame_state.frame_crc << 1;
        }
    }
}

// 解析头部信息
static char ymodem_parser_head(uint8_t  *buf, uint32_t sz ) //解析出头包中的文件名和大小
{

    uint8_t *fil_nm;
    uint8_t   fil_nm_len;
    uint32_t fil_sz;
    fil_nm = buf;
    fil_nm_len = strlen((const char *) fil_nm );
    fil_sz = (uint32_t)str_to_u32( (char *)buf+fil_nm_len+1);
    memcpy(g_modem_rx_packet.packet_file.name_data,fil_nm,fil_nm_len);
    g_modem_rx_packet.packet_file.name_data_size = fil_nm_len;
    g_modem_rx_packet.packet_file.file_size = fil_sz;
    if(fil_sz == 0 || fil_nm_len==0) {
        return -1;
    }
    return 0;
}


// 解析器初始化
static void parser_frame_reset(void)
{
    g_frame_state.frame_state = FRAME_FIND_HEAD;
    g_frame_state.frame_data_now_index = 0;
}


// 解析数据帧
static char parser_frame(uint8_t data)
{
    switch(g_frame_state.frame_state)
    {
    case FRAME_FIND_HEAD:
        if(data ==  SOH || data == STX || data == EOT) {
            g_frame.head = data;
            g_frame_state.frame_state = FRAME_FIND_INDEX;
            if(data ==  SOH) {
                g_frame_state.frame_data_len = SOH_PACKET_SIZE;
            }
            if(data == STX) {
                g_frame_state.frame_data_len = STX_PACKET_SIZE;
            }
            if(data == EOT) {
                g_frame_state.frame_data_len = 0;
                g_frame_state.frame_state = 0;
                return FRAME_PARSER_OK;
            }
        } else {
            return FRAME_PARSER_HEAE_ERROR;
        }
        break;
    case FRAME_FIND_INDEX:
        g_frame.index = data;
        g_frame_state.frame_state = FRAME_FIND_REINDEX;
        break;
    case FRAME_FIND_REINDEX:
        if((uint8_t)(~data) == g_frame.index) {
            g_frame.re_index = data;
            g_frame_state.frame_state = FRAME_FIND_DATA;

        } else {
            return FRAME_PARSER_REINDEX_NOTRQ_ERROR;
        }
        break;
    case FRAME_FIND_DATA:
        // add data
        if(g_frame_state.frame_data_now_index==g_frame_state.frame_data_len) {
            g_frame.crc_H = data;
            g_frame_state.frame_state = FRAME_FIND_CRC;
            return FRAME_PARSER_RUN;
        }
        g_frame.data[g_frame_state.frame_data_now_index++] = data;
        break;
    case FRAME_FIND_CRC:
        g_frame.crc_L = data;
        // reset crc
        crc16_reset();
        // calc crc
        for(int i = 0; i<g_frame_state.frame_data_len; i++) {
            crc16_sum(g_frame.data[i]);
        }
        // frame reset
        parser_frame_reset();
        if(((g_frame.crc_H<<8)+g_frame.crc_L) == g_frame_state.frame_crc) {
            return FRAME_PARSER_OK;
        }
        return FRAME_PARSER_CRC_ERROR;
    }
    return FRAME_PARSER_RUN;
}


/// ------------- sent



// 初始化发送
void ymodem_tx_init(char *file_name,char file_name_len,uint32_t file_size)
{
		// cpy file name
	memcpy(g_modem_tx_packet.packet_file.name_data,file_name,file_name_len);
	g_modem_tx_packet.packet_file.name_data_size = file_name_len;
	g_modem_tx_packet.packet_file.file_size = file_size;
}


// tx 数据包
static char ymodem_tx_packet(uint8_t data)
{
    switch(g_modem_tx_packet.packet_state)
    {
    // wait C
    case PACKET_TX_WAIT:
        if(data == CNC) {
						g_modem_tx_packet.now_packet_index = 1;
            g_modem_tx_packet.packet_file.remain_file_size = g_modem_tx_packet.packet_file.file_size;
            ymodem_tx_head_packet(g_tx_buff,g_modem_tx_packet.packet_file.name_data,g_modem_tx_packet.packet_file.name_data_size, g_modem_tx_packet.packet_file.file_size,PACKET_SIZE);
            g_modem_tx_packet.packet_state = PACKET_TX_WAIT_HEAD_ACK;
            return 0;
        }
        break;
    // wait head ack
    case PACKET_TX_WAIT_HEAD_ACK:
        if(data == ACK) {
            g_modem_tx_packet.packet_state = PACKET_TX_WAIT_SENT_DATA_ACK;
        }
        break;
    // sent data
    case PACKET_TX_WAIT_SENT_DATA_ACK:
        if((data == CNC && g_modem_tx_packet.now_packet_index ==1|| data == ACK)) {
						
						
            uint16_t packet_sent_size = ymodem_tx_data_packet(&g_modem_tx_packet.packet_file.file_ptr,g_tx_buff,g_modem_tx_packet.now_packet_index, g_modem_tx_packet.packet_file.remain_file_size,PACKET_SENT_MODE_AUTO);
            //g_modem_tx_packet.packet_file.file_ptr += packet_sent_size;
						g_modem_tx_packet.packet_file.sent_rec_file_size += packet_sent_size;
            g_modem_tx_packet.packet_file.remain_file_size -= g_modem_tx_packet.packet_file.sent_rec_file_size;

						g_modem_tx_packet.packet_file.percent = g_modem_tx_packet.packet_file.sent_rec_file_size *100/g_modem_tx_packet.packet_file.file_size;
            if(g_modem_tx_packet.packet_file.remain_file_size<=0) {
                g_modem_tx_packet.packet_state = PACKET_TX_WAIT_END_DATA_ACK;
                return 0;
            }
            g_modem_tx_packet.now_packet_index++;
            return 0;
        }
        break;
    case PACKET_TX_WAIT_END_DATA_ACK:
        if(data == ACK) {
            g_modem_tx_packet.packet_state=PACKET_TX_WAIT_FIRST_EOT_ACK_NCK;
            drv_com1_write(EOT);
            return 0;
        }

        break;
    case PACKET_TX_WAIT_FIRST_EOT_ACK_NCK:
        if(data == NAK) {
            g_ymodem.ymodem_write_byte(EOT);
            g_modem_tx_packet.packet_state = PACKET_TX_WAIT_EOT_WAIT_ACK;
        } else if(data == ACK) {
            g_modem_tx_packet.packet_state = PACKET_TX_WAIT_END_C;
        } else {
            printf("sent error \r\n");
        }
        break;
    case PACKET_TX_WAIT_EOT_WAIT_ACK:
        if(data == ACK) {
            g_modem_tx_packet.packet_state = PACKET_TX_WAIT_END_C;
        }
        break;
    case PACKET_TX_WAIT_END_C:
        if(data == CNC) {
            ymodem_tx_end_packet(g_tx_buff);
						printf("sent end\r\n");
        }
        break;
    default:
        break;
    }

}

// 轮询处理
void ymodem_tx_handle(uint8_t  *buf, uint32_t sz)
{
    for(int i = 0; i<sz; i++) {
        ymodem_tx_packet(buf[i]);
    }
}



// 打包发送头
void ymodem_tx_head_packet(uint8_t *p_data, const uint8_t *p_file_name,uint32_t file_name_length, uint32_t length,uint32_t  packet_size)
{
    uint32_t i, j = 0;
    uint8_t astring[10];

    /* first 3 bytes are constant */
    p_data[0] = SOH;
    p_data[1] = 0x00;
    p_data[2] = 0xff;

    /* Filename written */
    for (i = 0; (p_file_name[i] != '\0') && (i < file_name_length); i++)
    {
        p_data[i + 3] = p_file_name[i];
    }

    p_data[i + 3] = 0x00;

    /* file size written */
    Int2Str (astring, length);
    i = i + 3 + 1;
    while (astring[j] != '\0')
    {
        p_data[i++] = astring[j++];
    }
    /* padding with zeros */
    for (j = i; j < packet_size + 3; j++)
    {
        p_data[j] = 0;
    }
    // crc data
    unsigned short  mcrc = crc16(p_data+3,j-3);
    p_data[j] = mcrc>>8;
    p_data[j+1] = mcrc;
    for(int i = 0; i< packet_size + 5; i++) {
        g_ymodem.ymodem_write_byte(p_data[i]);
    }
}

// end packet
static void ymodem_tx_end_packet(uint8_t *p_data)
{
    uint32_t i = 0;
    /* first 3 bytes are constant */
    p_data[0] = 0x01;
    p_data[1] = 0x00;
    p_data[2] = 0xff;

    /* Filename written */
    for (i = 0; i<130; i++)
    {
        p_data[i + 3] = 0;
    }
    for(int i = 0; i< PACKET_SIZE + 5; i++) {
        g_ymodem.ymodem_write_byte(p_data[i]);
    }
}




// 打包发送数据包
static uint32_t ymodem_tx_data_packet(uint8_t **p_source, uint8_t *p_packet, uint8_t pkt_nr, uint32_t size_blk, uint8_t sent_mode)
{
    uint8_t *p_record;
    uint32_t i, size, packet_size;
    unsigned short mcrc = 0;

    // 剩余的数据用128字节还是1K字节传输
    packet_size = size_blk >= PACKET_1K_SIZE ? PACKET_1K_SIZE : PACKET_SIZE;
		// 系统调用读取函数
		
    // 剩余的数据需要的填充
    size = size_blk < packet_size ? size_blk : packet_size;

		
	  g_ymodem.ymodem_tx_data_handle(p_source, size,g_modem_tx_packet.packet_file.sent_rec_file_size,g_modem_tx_packet.packet_file.remain_file_size,g_modem_tx_packet.packet_file.percent);
		printf("--:P addr:%x\r\n",*p_source);
    // 1K字节传输
    if (packet_size == PACKET_1K_SIZE)
    {
        p_packet[0] = STX;
    }
    else
    {
        p_packet[0] = SOH;
    }
    // 填充帧头
    p_packet[1] = pkt_nr;
    p_packet[2] = (~pkt_nr);
    p_record = *p_source;

    // 填充数据
    for (i = 3; i < size + 3; i++)
    {
        p_packet[i] = *p_record++;
    }
    if ( size  <= packet_size)
    {
        for (i = size + 3; i < packet_size + 3; i++)
        {
            p_packet[i] = 0x1A; /* EOF (0x1A) or 0x00 */
        }
    }

    // 计算CRC
    mcrc = crc16(p_packet+3,i-3);
    p_packet[i] = mcrc>>8;
    p_packet[i+1] = mcrc&0xff;

    for(int i = 0; i<packet_size + 5; i++) {
        g_ymodem.ymodem_write_byte(p_packet[i]);
    }
    return packet_size;
}


