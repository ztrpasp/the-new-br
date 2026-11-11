#include "rte_CoLoR.h"

static void show_ANN(uint8_t *buf);
static void show_GET(uint8_t *buf);
static void show_DATA(uint8_t *buf);

/* ------------------------------construction of packet---------------------------------- */

uint16_t
calculate_cksum(uint16_t *buf, int count)
{
    uint32_t sum = 0;
    while(count > 1)
    {
        sum += *buf++;
        count -= 2;
    }
    if(count > 0)
        sum += *(uint8_t *)buf;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum>>16);
    return (uint16_t)(~sum);
}

// 1 for good, 0 for bad
static uint8_t
check_cksum(uint16_t *buf, int count)
{
    uint32_t sum = 0;
    while(count > 1)
    {
        sum += *buf++;
        count -= 2;
    }
    if(count > 0)
        sum += *(uint8_t *)buf;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum>>16);
    return (sum == 0xffff) ? 1 : 0;
}

// initialize the front part of the ANN pkt
void
ANN_init(uint8_t *buf)
{
    if(!buf)
        return;

    ANN_front_t *ann_pt = (ANN_front_t *)buf;
    ann_pt->version_package = 0x71;
    ann_pt->ttl = DEFAULT_TTL;
    ann_pt->pkg_len = sizeof(ANN_front_t);
    ann_pt->checksum = 0;
    ann_pt->flag = 0;
    ann_pt->unit_px_num = 0;
}

int
ANN_update_checksum(uint8_t *buf)
{
    if(!buf)
        return -1;

    ANN_front_t *ann_pt = (ANN_front_t *)buf;
    ann_pt->checksum = 0;
    ann_pt->checksum = calculate_cksum((uint16_t *)buf, ann_pt->pkg_len);
    return 1;
}

int
ANN_check_checksum(uint8_t *buf)
{
    if(!buf)
        return -1;

    ANN_front_t *ann_pt = (ANN_front_t *)buf;
    return check_cksum((uint16_t *)buf, ann_pt->pkg_len);
}

// append a unit to ANN
void
ANN_append_unit(uint8_t *buf, uint8_t *unit)
{
    if(!buf || !unit)
        return;

    ANN_front_t *ann_pt = (ANN_front_t *) buf;
    ANN_unit_front_t *new_unit = (ANN_unit_front_t *) unit;

    // copy unit to ann tail
    rte_memcpy(buf + ann_pt->pkg_len, (uint8_t *)new_unit, new_unit->unit_len);

    ann_pt->pkg_len += new_unit->unit_len;
    ADD_ANN_UNIT_NUM(ann_pt->unit_px_num);
}

// initialize a unit, with SID and nid
void
ANN_unit_construct(uint8_t *buf, uint8_t *n_sid, uint8_t *l_sid, uint8_t *nid)
{
    if(!buf)
        return;

    ANN_unit_front_t *unit_pt = (ANN_unit_front_t *) buf;
    uint8_t offset = sizeof(ANN_unit_front_t);
    unit_pt->flag = 0;
    unit_pt->strategy_num = 0;

    // append SID and NID
    if(NULL != n_sid)
    {
        SET_ANN_UNIT_FLAG_N(unit_pt->flag);
        rte_memcpy(buf + offset, n_sid, COLOR_N_SID_LEN);
        offset += COLOR_N_SID_LEN;
    }

    if(NULL != l_sid)
    {
        SET_ANN_UNIT_FLAG_L(unit_pt->flag);
        rte_memcpy(buf + offset, l_sid, COLOR_L_SID_LEN);
        offset += COLOR_L_SID_LEN;
    }

    if(NULL != nid)
    {
        SET_ANN_UNIT_FLAG_I(unit_pt->flag);
        rte_memcpy(buf + offset, nid, COLOR_NID_LEN);
        offset += COLOR_NID_LEN;
    }

    unit_pt->unit_len = offset;
}

// append strategy unit to unit buf
void
ANN_unit_append_strategy(uint8_t *buf, uint8_t t, uint8_t l, uint8_t *v)
{
    if(!buf || l <= 0 || !v)
        return;

    ANN_unit_front_t *unit_pt = (ANN_unit_front_t *) buf;
    uint8_t offset = unit_pt->unit_len;


    *((uint8_t *)(buf + offset)) = t; // t
    *((uint8_t *)(buf + offset + 1)) = l; // l

    rte_memcpy(buf + offset + 2, v, l); // v

    unit_pt->unit_len += 2 + l;
    unit_pt->strategy_num += 1;
}

// append PX to the tail of ANN
void
ANN_append_px(uint8_t *buf, uint16_t px)
{
    if(!buf) return;

    ANN_front_t *ann_t = (ANN_front_t *)buf;
    *(uint16_t *)(buf + ann_t->pkg_len) = px;
    ann_t->pkg_len += 2;
    ADD_ANN_PX_NUM(ann_t->unit_px_num);
}

// initialize the front of the GET pkt
void
GET_init(uint8_t *buf, uint8_t *n_sid, uint8_t *l_sid, uint8_t *nid)
{
    if(!buf || !n_sid || !l_sid || !nid)
        return;

    GET_front_t *get_pt = (GET_front_t *)buf;
    get_pt->version_package = 0x72;
    get_pt->ttl = DEFAULT_TTL;
    get_pt->pkg_len = sizeof(GET_front_t);
    get_pt->checksum = 0;
    get_pt->MTU = 0;
    get_pt->PID_num = 0;
    get_pt->flag = 0;
    get_pt->Minimal_PID_CP = 0;

    rte_memcpy(get_pt->N_sid, n_sid, COLOR_N_SID_LEN);
    rte_memcpy(get_pt->L_sid, l_sid, COLOR_L_SID_LEN);
    rte_memcpy(get_pt->nid, nid, COLOR_NID_LEN);

}

// append PID to the tail of GET
void
GET_append_pid(uint8_t *buf, uint32_t pid)
{
    if(!buf)
        return;

    GET_front_t *get_pt = (GET_front_t *)buf;
    *(uint32_t *)(buf + get_pt->pkg_len) = pid;
    get_pt->pkg_len += 4;
    get_pt->PID_num += 1;
}

void
GET_update_checksum(uint8_t *buf)
{
    if(!buf)
        return;

    GET_front_t *get_pt = (GET_front_t *)buf;
    get_pt->checksum = 0;
    get_pt->checksum = calculate_cksum((uint16_t *)buf, get_pt->pkg_len);
}

int
GET_check_checksum(uint8_t *buf)
{
    if(!buf)
        return -1;

    GET_front_t *get_pt = (GET_front_t *)buf;
    return check_cksum((uint16_t *)buf, get_pt->pkg_len);
}

// initialize the front of the DATA pkt
void
DATA_init(uint8_t *buf, uint8_t *n_sid, uint8_t *l_sid, uint8_t *nid)
{
    if(!buf || !n_sid || !l_sid || !nid)
        return;

    DATA_front_t *data_pt = (DATA_front_t *)buf;
    data_pt->version_package = 0x73;
    data_pt->ttl = DEFAULT_TTL;
    data_pt->pkg_len = sizeof(DATA_front_t);
    data_pt->checksum = 0;
    data_pt->header_len = data_pt->pkg_len;
    data_pt->PID_pt = 0;
    data_pt->PID_num = 0;
    data_pt->flag = 0;

    rte_memcpy(data_pt->N_sid, n_sid, COLOR_N_SID_LEN);
    rte_memcpy(data_pt->L_sid, l_sid, COLOR_L_SID_LEN);
    rte_memcpy(data_pt->nid, nid, COLOR_NID_LEN);

}

// append PID to the tail of DATA
// must consider about data behind header!
void
DATA_append_pid(uint8_t *buf, uint32_t pid)
{
    if(!buf)
        return;

    DATA_front_t *data_pt = (DATA_front_t *)buf;
    *(uint32_t *)(buf + data_pt->pkg_len) = pid;
    data_pt->pkg_len += 4;
    data_pt->header_len = data_pt->pkg_len;
    data_pt->PID_pt += 1;
    data_pt->PID_num += 1;
}

// append payload to DATA
void
DATA_append_data(uint8_t *buf, uint8_t *data, int len)
{
    if(!buf || !data || len <= 0)
        return;

    DATA_front_t *data_pt = (DATA_front_t *)buf;


    rte_memcpy(buf + data_pt->pkg_len, data, len);

    data_pt->pkg_len += len;
}

void
DATA_update_checksum(uint8_t *buf)
{
    if(!buf)
        return;

    DATA_front_t *data_pt = (DATA_front_t *)buf;
    data_pt->checksum = 0;
    data_pt->checksum = calculate_cksum((uint16_t *)buf, data_pt->header_len);
}

int
DATA_check_checksum(uint8_t *buf)
{
    if(!buf)
        return -1;

    DATA_front_t *data_pt = (DATA_front_t *)buf;
    return check_cksum((uint16_t *)buf, data_pt->header_len);
}

/* ------------------------------tool function---------------------------------- */

// get package protocol
int
CoLoR_get_pkg_protocol(uint8_t *buf)
{
    if(!buf)
        return -1;
    else
        return GET_PROTOCOL_TYPE(buf[0]);
}

// get package type
int
CoLoR_get_pkg_type(uint8_t *buf)
{
    if(!buf)
        return -1;
    else
        return GET_PACKAGE_TYPE(buf[0]);
}

// get package length
int
CoLoR_get_pkg_length(uint8_t *buf)
{
    switch(CoLoR_get_pkg_type(buf))
    {
        case COLOR_ANN_TYPE:
        {
            return (((ANN_front_t *)buf))->pkg_len;
        }
        case COLOR_GET_TYPE:
        {
            return (((GET_front_t *)buf))->pkg_len;
        }
        case COLOR_DATA_TYPE:
        {
            return (((DATA_front_t *)buf))->pkg_len;
        }
        default: return -1;
    }
}

/* ------------------------------show function---------------------------------- */

void
show_segment(uint8_t *buf, int len)
{
    int i;
    printf("0x");
    for(i=0; i<len; i++)
    {
        printf("%02x", *(uint8_t *)(buf + i));
    }
    printf("\r\n");
}

void print_hex(uint8_t *buf, int len)
{
    int i;
    printf("0x");
    for(i=0; i<len; i++)
    {
        printf("%02X", *(uint8_t *)(buf + i));
    }
}

// show color package
void
CoLoR_show_pkg(uint8_t *buf)
{
    switch(CoLoR_get_pkg_type(buf))
    {
        case COLOR_ANN_TYPE:
        {
            return show_ANN(buf);
        }
        case COLOR_GET_TYPE:
        {
            return show_GET(buf);
        }
        case COLOR_DATA_TYPE:
        {
            return show_DATA(buf);
        }
        default: ;
    }
}

static void
show_ANN(uint8_t *buf)
{
    if(!buf) return;

    ANN_front_t *ann_pt = (ANN_front_t *)buf;
    ANN_unit_front_t *unit_pt;

    int i, j, offset = sizeof(ANN_front_t);
    int unit_num = GET_ANN_UNIT_NUM(ann_pt->unit_px_num),
        px_num = GET_ANN_PX_NUM(ann_pt->unit_px_num);
    uint8_t s_len;

    // show the header of ANN packet
    printf("ANN packet:\r\n"
        "\tVersion: %d, Type: %d, Hop Limit: %d, Total Length: %d\r\n"
        "\tChecksum: 0x%04x, Flags: 0x%02x, Unit Num: %d, PX Num: %d\r\n",
        ann_pt->version_package >> 4, ann_pt->version_package & 0x0F, ann_pt->ttl, ann_pt->pkg_len,
        ann_pt->checksum, ann_pt->flag, ann_pt->unit_px_num >> 4, ann_pt->unit_px_num & 0x0F);

    // show the units in ANN
    for(i = 0; i < unit_num ; i++)
    {
        unit_pt = (ANN_unit_front_t *) (buf + offset);
        printf("\tAnnounce Unit:\r\n"
            "\t\tflag: 0x%02x, unit_len: %d, strategy_num: %d\r\n",
            unit_pt->flag, unit_pt->unit_len, unit_pt->strategy_num);
        offset += sizeof(ANN_unit_front_t);

        // show sid/nid or not
        if(CHECK_ANN_UNIT_FLAG_N(unit_pt->flag))
        {
            printf("\t\tNSID: ");
            show_segment(buf + offset, COLOR_N_SID_LEN);
            offset += COLOR_N_SID_LEN;
        }
        if(CHECK_ANN_UNIT_FLAG_L(unit_pt->flag))
        {
            printf("\t\tLSID: ");
            show_segment(buf + offset, COLOR_L_SID_LEN);
            offset += COLOR_L_SID_LEN;
        }
        if(CHECK_ANN_UNIT_FLAG_I(unit_pt->flag))
        {
            printf("\t\tNID: ");
            show_segment(buf + offset, COLOR_NID_LEN);
            offset += COLOR_NID_LEN;
        }

        // show strategies in unit
        for(j=0; j < unit_pt->strategy_num; j++)
        {
            s_len = *(uint8_t *)(buf + offset + 1);
            printf("\t\tStrategy Unit:\r\n\t\t\ttag: %d\r\n\t\t\tlen: %d\r\n" ,
                    *(uint8_t *)(buf + offset),
                    s_len);
            printf("\t\t\tvalue: ");
            show_segment(buf + offset + 2, s_len);
            offset += 2 + s_len;
        }
    }

    // show px
    for(i = 0; i < px_num; i++)
    {
        printf("\tPX: 0x%04x0000/16\r\n", *(uint16_t *)(buf + offset + i * 2));
    }
}

static void
show_GET(uint8_t *buf)
{
    if(!buf) return;

    int i;
    GET_front_t *get_pt = (GET_front_t *)buf;

    printf("GET packet:\r\n"
        "\tVersion: %d, Type: %d, Hop Limit: %d, Total Length: %d\r\n"
        "\tChecksum: 0x%04x, MTU: %d\r\n"
        "\tNumber of PIDs: %d, Flags: 0x%02x, Minimal_PID_CP: %d\r\n",
        get_pt->version_package >> 4, get_pt->version_package & 0x0F, get_pt->ttl, get_pt->pkg_len,
        get_pt->checksum, get_pt->MTU,
        get_pt->PID_num, get_pt->flag, get_pt->Minimal_PID_CP);

    printf("\tNSID: ");show_segment(get_pt->N_sid, COLOR_N_SID_LEN);
    printf("\tLSID: ");show_segment(get_pt->L_sid, COLOR_L_SID_LEN);
    printf("\tNID: ");show_segment(get_pt->nid, COLOR_NID_LEN);

    for(i=0; i < get_pt -> PID_num; i++)
    {
        uint32_t *p = (uint32_t *)(buf + sizeof(GET_front_t) + i * 4);
        printf("\tPID: 0x%08x\r\n", *p);
    }

}

static void
show_DATA(uint8_t *buf)
{
    if(!buf) return;

    DATA_front_t *data_pt = (DATA_front_t *)buf;

    printf("\tVersion: %d, Type: %d, Hop Limit: %d, Total Length: %d\r\n"
        "\tChecksum: 0x%04x, Header Length: %d, PID Pointer: %d\r\n"
        "\tNumber of PIDs: %d, Flags: 0x%02x\r\n",
        data_pt->version_package >> 4, data_pt->version_package & 0x0F, data_pt->ttl, data_pt->pkg_len, 
        data_pt->checksum, data_pt->header_len, data_pt->PID_pt, 
        data_pt->PID_num, data_pt->flag);

    printf("\tNSID: "); show_segment(data_pt->N_sid, COLOR_N_SID_LEN);
    printf("\tLSID: "); show_segment(data_pt->L_sid, COLOR_L_SID_LEN);
    printf("\tDestination NID: "); show_segment(data_pt->nid, COLOR_NID_LEN);

    int offset = COLOR_NID_LEN;
    if (COLOR_DATA_FLAG_B(data_pt->flag) || COLOR_DATA_FLAG_R(data_pt->flag)) {
        printf("\tSource NID: "); show_segment(data_pt->nid + offset, COLOR_NID_LEN);
        offset += COLOR_NID_LEN;
    }
    if (COLOR_DATA_FLAG_Q(data_pt->flag)) {
        int qos_len = *(data_pt->nid + offset);
        printf("\tQoS Length: %d; QoS: ", qos_len); print_hex(data_pt->nid + offset + 1, qos_len / 8);
        printf("\r\n");
        offset += 1 + qos_len / 8; // qos_len is in bits
    }
    if (COLOR_DATA_FLAG_C(data_pt->flag)) {
        printf("\tHMAC: 0x%08x\r\n", *(uint32_t *)(data_pt->nid + offset));
        offset += 4;
    }
    if (COLOR_DATA_FLAG_S(data_pt->flag)) {
        printf("\tSegment ID: 0x%08x\r\n", *(uint32_t *)(data_pt->nid + offset));
        offset += 4;
    }
    printf("\tPID Sequence: ");
    for (int i = 0; i < data_pt->PID_num; i++) {
        printf("0x%08x, ", *(uint32_t *)(data_pt->nid + offset));
        offset += 4;
    }
    printf("\r\n");
    if (COLOR_DATA_FLAG_R(data_pt->flag)) {
        printf("\tReserved PID: 0x%08x\r\n", *(uint32_t *)(data_pt->nid + offset));
        offset += COLOR_NID_LEN;
    }
}
