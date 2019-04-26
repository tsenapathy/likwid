/*
 * =======================================================================================
 *
 *      Filename:  frequency.c
 *
 *      Description:  Module implementing an interface for frequency manipulation
 *
 *      Version:   <VERSION>
 *      Released:  <DATE>
 *
 *      Author:   Thomas Roehl (tr), thomas.roehl@googlemail.com
 *                Jan Treibig (jt), jan.treibig@gmail.com
 *      Project:  likwid
 *
 *      Copyright (C) 2016 RRZE, University Erlangen-Nuremberg
 *
 *      This program is free software: you can redistribute it and/or modify it under
 *      the terms of the GNU General Public License as published by the Free Software
 *      Foundation, either version 3 of the License, or (at your option) any later
 *      version.
 *
 *      This program is distributed in the hope that it will be useful, but WITHOUT ANY
 *      WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 *      PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License along with
 *      this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * =======================================================================================
 */


#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <error.h>
#include <string.h>

#include <bstrlib.h>
#include <likwid.h>
#include <topology.h>
#include <access.h>
#include <registers.h>
#include <lock.h>
#include <configuration.h>

#include <frequency.h>
#include <frequency_client.h>
/*#include <frequency_acpi.h>*/
/*#include <frequency_pstate.h>*/

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#if !defined(__ARM_ARCH_7A__) && !defined(__ARM_ARCH_8A)
#include <cpuid.h>
#endif


void (*freq_init)() = NULL;
int (*freq_send)(FreqDataRecordType type, FreqDataRecordLocation loc, int cpu, int len, char* data) = NULL;
void (*freq_finalize)() = NULL;
static int freq_initialized = 0;
static int own_hpm = 0;

static struct cpufreq_files* cpufiles = NULL;


static int fsocket = -1;

struct cpufreq_files {
    int  cur_freq;
    int  max_freq;
    int  min_freq;
    int  avail_freq;
    int  avail_govs;
    int  driver;
    int  set_freq;
    int  set_gov;
    int  conf_max_freq;
    int  conf_min_freq;
};

char* cpufreq_files[] ={
    "scaling_cur_freq",
    "scaling_max_freq",
    "scaling_min_freq",
    "scaling_available_frequencies",
    "scaling_available_governors",
    "scaling_driver",
    "scaling_setspeed",
    "scaling_governor",
    "cpuinfo_max_freq",
    "cpuinfo_min_freq",
    NULL,
};

char* pstate_files[] ={
    "scaling_cur_freq",
    "scaling_max_freq",
    "scaling_min_freq",
    NULL,
    "scaling_available_governors",
    "scaling_driver",
    "scaling_setspeed",
    "scaling_governor",
    NULL,
};

static void close_cpu(struct cpufreq_files* cpufiles)
{
    if (cpufiles)
    {
        if (cpufiles->cur_freq >= 0)
        {
            //printf( "Close cur_freq %d\n", cpufiles->cur_freq);
            close(cpufiles->cur_freq);
            cpufiles->cur_freq = -1;
        }
        if (cpufiles->max_freq >= 0)
        {
            //printf( "Close max_freq %d\n", cpufiles->cur_freq);
            close(cpufiles->max_freq);
            cpufiles->max_freq = -1;
        }
        if (cpufiles->min_freq >= 0)
        {
            //printf( "Close min_freq %d\n", cpufiles->cur_freq);
            close(cpufiles->min_freq);
            cpufiles->min_freq = -1;
        }
        if (cpufiles->set_freq >= 0)
        {
            //printf( "Close set_freq %d\n", cpufiles->cur_freq);
            close(cpufiles->set_freq);
            cpufiles->set_freq = -1;
        }
        if (cpufiles->set_gov >= 0)
        {
            //printf( "Close set_gov %d\n", cpufiles->cur_freq);
            close(cpufiles->set_gov);
            cpufiles->set_gov = -1;
        }
        if (cpufiles->avail_freq >= 0)
        {
            //printf( "Close avail_freq %d\n", cpufiles->cur_freq);
            close(cpufiles->avail_freq);
            cpufiles->avail_freq = -1;
        }
        if (cpufiles->avail_govs >= 0)
        {
            //printf( "Close avail_govs %d\n", cpufiles->cur_freq);
            close(cpufiles->avail_govs);
            cpufiles->avail_govs = -1;
        }
        if (cpufiles->driver >= 0)
        {
            //printf( "Close driver %d\n", cpufiles->cur_freq);
            close(cpufiles->driver);
            cpufiles->driver = -1;
        }
        if (cpufiles->conf_min_freq >= 0)
        {
            //printf( "Close conf_min_freq %d\n", cpufiles->conf_min_freq);
            close(cpufiles->conf_min_freq);
            cpufiles->conf_min_freq = -1;
        }
        if (cpufiles->conf_max_freq >= 0)
        {
            //printf( "Close conf_max_freq %d\n", cpufiles->conf_max_freq);
            close(cpufiles->conf_max_freq);
            cpufiles->conf_max_freq = -1;
        }
    }
}

static int open_cpu_file(char* filename, int* fd)
{
    int f = -1;
    int access_flag = R_OK|W_OK;
    int open_flag = O_RDWR;

    f = open(filename, open_flag);
    if (f < 0)
    {
        open_flag = O_RDONLY;
        f = open(filename, open_flag);
        if (f < 0)
        {
            DEBUG_PRINT(DEBUGLEV_DEVELOP, "Failed to open file %s \n", filename);
            *fd = -1;
            return 0;
        }
    }
    *fd = f;
    //printf( "Opened %s %s = %d\n", filename, (open_flag == O_RDONLY ? "readable" : "writable"), *fd);
    return 0;
}

static int open_cpu(int cpu, struct cpufreq_files* files)
{
    char dname[1025];
    char fname[1025];

    FILE* fp = NULL;
    if (cpu >= 0)
    {
        int ret = snprintf(dname, 1024, "/sys/devices/system/cpu/cpu%d/cpufreq", cpu);
        if (ret > 0)
        {
            dname[ret] = '\0';
        }
        else
        {
            return -1;
        }


        ret = snprintf(fname, 1024, "%s/%s", dname, "scaling_cur_freq");
        if (ret > 0)
        {
            fname[ret] = '\0';
            if (open_cpu_file(fname, &files->cur_freq) < 0)
            {
                goto cleanup;
            }
        }
        ret = snprintf(fname, 1024, "%s/%s", dname, "scaling_max_freq");
        if (ret > 0)
        {
            fname[ret] = '\0';
            if (open_cpu_file(fname, &files->max_freq) < 0)
            {
                goto cleanup;
            }
        }
        ret = snprintf(fname, 1024, "%s/%s", dname, "scaling_min_freq");
        if (ret > 0)
        {
            fname[ret] = '\0';
            if (open_cpu_file(fname, &files->min_freq) < 0)
            {
                goto cleanup;
            }
        }
        ret = snprintf(fname, 1024, "%s/%s", dname, "scaling_setspeed");
        if (ret > 0)
        {
            fname[ret] = '\0';
            if (open_cpu_file(fname, &files->set_freq) < 0)
            {
                goto cleanup;
            }
        }
        ret = snprintf(fname, 1024, "%s/%s", dname, "scaling_governor");
        if (ret > 0)
        {
            fname[ret] = '\0';
            if (open_cpu_file(fname, &files->set_gov) < 0)
            {
                goto cleanup;
            }
        }
        ret = snprintf(fname, 1024, "%s/%s", dname, "scaling_available_governors");
        if (ret > 0)
        {
            fname[ret] = '\0';
            if (open_cpu_file(fname, &files->avail_govs) < 0)
            {
                goto cleanup;
            }
        }
        ret = snprintf(fname, 1024, "%s/%s", dname, "scaling_available_frequencies");
        if (ret > 0)
        {
            fname[ret] = '\0';
            if (open_cpu_file(fname, &files->avail_freq) < 0)
            {
                goto cleanup;
            }
        }
        ret = snprintf(fname, 1024, "%s/%s", dname, "scaling_driver");
        if (ret > 0)
        {
            fname[ret] = '\0';
            if (open_cpu_file(fname, &files->driver) < 0)
            {
                goto cleanup;
            }
        }
        ret = snprintf(fname, 1024, "%s/%s", dname, "cpuinfo_min_freq");
        if (ret > 0)
        {
            fname[ret] = '\0';
            if (open_cpu_file(fname, &files->conf_min_freq) < 0)
            {
                goto cleanup;
            }
        }
        ret = snprintf(fname, 1024, "%s/%s", dname, "cpuinfo_max_freq");
        if (ret > 0)
        {
            fname[ret] = '\0';
            if (open_cpu_file(fname, &files->conf_max_freq) < 0)
            {
                goto cleanup;
            }
        }
        return 0;
    }
cleanup:
    close_cpu(files);
    return -1;
}


static int
freq_client_startDaemon()
{
    /* Check the function of the daemon here */
    int res = 0;
    char* filepath;
    char *newargv[] = { NULL };
    char *newenv[] = { NULL };
    //char *safeexeprog = TOSTRING(FREQDAEMON);
    char *exeprog = TOSTRING(FREQDAEMON);
    struct sockaddr_un address;
    size_t address_length;
    int  ret;
    pid_t pid;
    int timeout = 1000;
    int socket_fd = -1;
    int print_once = 0;

    if (access(exeprog, X_OK))
    {
        fprintf(stderr, "Failed to find the daemon '%s'\n", exeprog);
        exit(EXIT_FAILURE);
    }
    DEBUG_PRINT(DEBUGLEV_INFO, Starting daemon %s, exeprog);
    pid = fork();

    if (pid == 0)
    {
/*        Remove pinning here and delay it until first read or write call to check*/
/*        if we are running in a multi-threaded environment.*/
/*        if (cpu_id >= 0)*/
/*        {*/
/*            cpu_set_t cpuset;*/
/*            CPU_ZERO(&cpuset);*/
/*            CPU_SET(cpu_id, &cpuset);*/
/*            sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);*/
/*        }*/
        ret = execve (exeprog, newargv, newenv);

        if (ret < 0)
        {
            //ERRNO_PRINT;
            fprintf(stderr, "Failed to execute the daemon '%s'\n", exeprog);
            exit(EXIT_FAILURE);
        }
    }
    else if (pid < 0)
    {
        fprintf(stderr, "Failed to fork frequency daemon");
        return pid;
    }

    EXIT_IF_ERROR(socket_fd = socket(AF_LOCAL, SOCK_STREAM, 0), socket() failed);

    address.sun_family = AF_LOCAL;
    address_length = sizeof(address);
    snprintf(address.sun_path, sizeof(address.sun_path), TOSTRING(LIKWIDSOCKETBASE) "-freq-%d", pid);
    filepath = strdup(address.sun_path);

    res = connect(socket_fd, (struct sockaddr *) &address, address_length);
    while (res && timeout > 0)
    {
        usleep(2500);
        res = connect(socket_fd, (struct sockaddr *) &address, address_length);

        if (res == 0)
        {
            break;
        }

        timeout--;
        //fprintf(stderr, "Still waiting for socket %s...\n", filepath);
    }

    if (timeout <= 0)
    {
        //ERRNO_PRINT;  /* should hopefully still work, as we make no syscalls in between. */
        fprintf(stderr, "Exiting due to timeout: The socket file at '%s' could not be\n", filepath);
        fprintf(stderr, "opened within 10 seconds. Consult the error message above\n");
        fprintf(stderr, "this to find out why. If the error is 'no such file or directoy',\n");
        fprintf(stderr, "it usually means that likwid-accessD just failed to start.\n");
        exit(EXIT_FAILURE);
    }
    DEBUG_PRINT(DEBUGLEV_DEVELOP, "Successfully opened socket %s to daemon\n", filepath);
    free(filepath);

    return socket_fd;
}


static void freq_init_direct()
{
    //printf("Calling %s\n", __func__);
    int threads = cpuid_topology.numHWThreads;
    cpufiles = malloc(threads* sizeof(struct cpufreq_files));
    if (!cpufiles)
    {
        fprintf(stderr,"Failed to allocate space\n");
        return;
    }
    for (int i=0;i<threads;i++)
    {
        memset(&cpufiles[i], -1, sizeof(struct cpufreq_files));
        int ret = open_cpu(i, &cpufiles[i]);
        if (ret < 0)
        {
            fprintf(stderr,"Failed to open files for CPU %d\n", i);
        }
    }
    return;
}

static int freq_send_direct(FreqDataRecordType type, FreqDataRecordLocation loc, int cpu, int len, char* data)
{
    //printf("Calling %s\n", __func__);
    int fd = -1;
    int ret = 0;
    int only_read = 0;
    struct cpufreq_files* f = &cpufiles[cpu];

    switch(loc)
    {
        case FREQ_LOC_CUR:
            fd = f->cur_freq;
            only_read = 1;
            DEBUG_PRINT(DEBUGLEV_DEVELOP, "CMD %s CPU %d FREQ_LOC_CUR FD %d\n", (type == FREQ_WRITE ? "WRITE" : "READ"), cpu, fd);
            break;
        case FREQ_LOC_MIN:
            fd = f->min_freq;
            DEBUG_PRINT(DEBUGLEV_DEVELOP, "CMD %s CPU %d FREQ_LOC_MIN FD %d\n", (type == FREQ_WRITE ? "WRITE" : "READ"), cpu, fd);
            break;
        case FREQ_LOC_MAX:
            fd = f->max_freq;
            DEBUG_PRINT(DEBUGLEV_DEVELOP, "CMD %s CPU %d FREQ_LOC_MAX FD %d\n", (type == FREQ_WRITE ? "WRITE" : "READ"), cpu, fd);
            break;
        case FREQ_LOC_GOV:
            fd = f->set_gov;
            DEBUG_PRINT(DEBUGLEV_DEVELOP, "CMD %s CPU %d FREQ_LOC_GOV FD %d\n", (type == FREQ_WRITE ? "WRITE" : "READ"), cpu, fd);
            break;
        case FREQ_LOC_AVAIL_GOV:
            fd = f->avail_govs;
            only_read = 1;
            DEBUG_PRINT(DEBUGLEV_DEVELOP, "CMD %s CPU %d FREQ_LOC_AVAIL_GOV FD %d\n", (type == FREQ_WRITE ? "WRITE" : "READ"), cpu, fd);
            break;
        case FREQ_LOC_AVAIL_FREQ:
            fd = f->avail_freq;
            only_read = 1;
            DEBUG_PRINT(DEBUGLEV_DEVELOP, "CMD %s CPU %d FREQ_LOC_AVAIL_FREQ FD %d\n", (type == FREQ_WRITE ? "WRITE" : "READ"), cpu, fd);
            break;
        case FREQ_LOC_CONF_MIN:
            fd = f->conf_min_freq;
            only_read = 1;
            DEBUG_PRINT(DEBUGLEV_DEVELOP, "CMD %s CPU %d FREQ_LOC_CONF_MIN FD %d\n", (type == FREQ_WRITE ? "WRITE" : "READ"), cpu, fd);
            break;
        case FREQ_LOC_CONF_MAX:
            fd = f->conf_max_freq;
            only_read = 1;
            DEBUG_PRINT(DEBUGLEV_DEVELOP, "CMD %s CPU %d FREQ_LOC_CONF_MAX FD %d\n", (type == FREQ_WRITE ? "WRITE" : "READ"), cpu, fd);
            break;
        default:
            fprintf(stderr,"Invalid location specified in record\n");
            break;
    }
    if (fd < 0)
        return -1;
    switch (type)
    {
        case FREQ_WRITE:
            if (only_read)
            {
                return -EPERM;
            }
            lseek(fd, 0, SEEK_SET);
            ret = write(fd, data, len);
            break;
        case FREQ_READ:
            lseek(fd, 0, SEEK_SET);
            ret = read(fd, data, len);
            break;
    }
    if (ret < 0)
        return ret;
    return 0;
}

static void freq_finalize_direct()
{
    //printf("Calling %s\n", __func__);
    int threads = cpuid_topology.numHWThreads;
    if (cpufiles)
    {
        for (int i=0;i<threads;i++)
        {
            close_cpu(&cpufiles[i]);
        }
        free(cpufiles);
        cpufiles = NULL;
    }
    return;
}

static void freq_init_client()
{
    //printf("Calling %s\n", __func__);
    fsocket = freq_client_startDaemon();
    return;
}

static int freq_send_client(FreqDataRecordType type, FreqDataRecordLocation loc, int cpu, int len, char* data)
{
    //printf("Calling %s\n", __func__);
    FreqDataRecord record;
    if (fsocket >= 0)
    {
        memset(&record, 0, sizeof(FreqDataRecord));
        record.type = type;
        record.loc = loc;
        record.cpu = cpu;
        record.errorcode = FREQ_ERR_NONE;
        snprintf(record.data, LIKWID_FREQUENCY_MAX_DATA_LENGTH, "%.*s", len, data);
        record.datalen = len;
        CHECK_ERROR(write(fsocket, &record, sizeof(FreqDataRecord)),socket write failed);
        CHECK_ERROR(read(fsocket, &record, sizeof(FreqDataRecord)), socket read failed);
        if (record.errorcode != FREQ_ERR_NONE)
        {
            
            switch(record.errorcode)
            {
                case FREQ_ERR_NOFILE:
                    fprintf(stderr, "Error %d: No such file\n", record.errorcode);
                    return -ENOENT;
                    break;
                case FREQ_ERR_NOPERM:
                    fprintf(stderr, "Error %d: No permission\n", record.errorcode);
                    return -EACCES;
                    break;
                case FREQ_ERR_UNKNOWN:
                    fprintf(stderr, "Error %d: Unknown error\n", record.errorcode);
                    return -EBADF;
                    break;
            }
            return -1;
        }
    }
    return 0;
}

static void freq_finalize_client()
{
    //printf("Calling %s\n", __func__);
    FreqDataRecord record;
    if (fsocket >= 0)
    {
        memset(&record, 0, sizeof(FreqDataRecord));
        record.type = FREQ_EXIT;
        CHECK_ERROR(write(fsocket, &record, sizeof(FreqDataRecord)),socket write failed);
        CHECK_ERROR(close(fsocket),socket close failed);
        fsocket = -1;
    }
    return;
}


static int getAMDTurbo(const int cpu_id)
{
    int err = 0;

    if (!lock_check())
    {
        fprintf(stderr,"Access to frequency backend is locked.\n");
        return 0;
    }

    if (!HPMinitialized())
    {
        HPMinit();
        own_hpm = 1;

        err = HPMaddThread(cpu_id);
        if (err != 0)
        {
            ERROR_PLAIN_PRINT(Cannot get access to MSRs)
            return err;
        }
    }
    else
    {
        err = HPMaddThread(cpu_id);
        if (err != 0)
        {
            ERROR_PLAIN_PRINT(Cannot get access to MSRs)
            return err;
        }
    }

    uint64_t tmp = 0x0ULL;
    err = HPMread(cpu_id, MSR_DEV, 0xC0010015, &tmp);
    if (err)
    {
        ERROR_PLAIN_PRINT(Cannot read register 0xC0010015);
        return err;
    }
    
    err = ((tmp >> 25) & 0x1);
    return err == 0;
}

static int setAMDTurbo(const int cpu_id, const int turbo)
{
    int err = 0;

    if (!lock_check())
    {
        fprintf(stderr,"Access to frequency backend is locked.\n");
        return -EPERM;
    }

    if (!HPMinitialized())
    {
        HPMinit();
        own_hpm = 1;

        err = HPMaddThread(cpu_id);
        if (err != 0)
        {
            ERROR_PLAIN_PRINT(Cannot get access to MSRs)
            return err;
        }
    }
    else
    {
        err = HPMaddThread(cpu_id);
        if (err != 0)
        {
            ERROR_PLAIN_PRINT(Cannot get access to MSRs)
            return err;
        }
    }

    uint64_t tmp = 0x0ULL;
    err = HPMread(cpu_id, MSR_DEV, 0xC0010015, &tmp);
    if (err)
    {
        ERROR_PLAIN_PRINT(Cannot read register 0xC0010015);
        return err;
    }

    if (turbo)
    {
        tmp &= ~(1ULL<<25);
    }
    else
    {
        tmp |= (1ULL << 25);
    }
    err = HPMwrite(cpu_id, MSR_DEV, 0xC0010015, tmp);
    if (err)
    {
        ERROR_PLAIN_PRINT(Cannot write register 0xC0010015);
        return err;
    }

    return err == 0;
}

static int getIntelTurbo(const int cpu_id)
{
    int err = 0;

    if (!lock_check())
    {
        fprintf(stderr,"Access to frequency backend is locked.\n");
        return 0;
    }

    if (!HPMinitialized())
    {
        HPMinit();
        own_hpm = 1;
        err = HPMaddThread(cpu_id);
        if (err != 0)
        {
            ERROR_PLAIN_PRINT(Cannot get access to MSRs)
            return err;
        }
    }
    else
    {
        err = HPMaddThread(cpu_id);
        if (err != 0)
        {
            ERROR_PLAIN_PRINT(Cannot get access to MSRs)
            return err;
        }
    }

    uint64_t tmp = 0x0ULL;
    err = HPMread(cpu_id, MSR_DEV, MSR_IA32_MISC_ENABLE, &tmp);
    if (err)
    {
        ERROR_PRINT(Cannot read register 0x%x, MSR_IA32_MISC_ENABLE);
        return err;
    }

    err = ((tmp >> 38) & 0x1);
    return err == 0;
}

static int setIntelTurbo(const int cpu_id, const int turbo)
{
    int err = 0;
    
    if (!lock_check())
    {
        fprintf(stderr,"Access to frequency backend is locked.\n");
        return -EPERM;
    }

    if (!HPMinitialized())
    {
        HPMinit();
        own_hpm = 1;

        err = HPMaddThread(cpu_id);
        if (err != 0)
        {
            ERROR_PLAIN_PRINT(Cannot get access to MSRs)
            return err;
        }
    }
    else
    {
        err = HPMaddThread(cpu_id);
        if (err != 0)
        {
            ERROR_PLAIN_PRINT(Cannot get access to MSRs)
            return err;
        }
    }

    uint64_t tmp = 0x0ULL;
    err = HPMread(cpu_id, MSR_DEV, MSR_IA32_MISC_ENABLE, &tmp);
    if (err)
    {
        ERROR_PRINT(Cannot read register 0x%x, MSR_IA32_MISC_ENABLE);
        return err;
    }
    if (turbo)
    {
        tmp &= ~(1ULL << 38);
    }
    else
    {
        tmp |= (1ULL << 38);
    }
    err = HPMwrite(cpu_id, MSR_DEV, MSR_IA32_MISC_ENABLE, tmp);
    if (err)
    {
        ERROR_PRINT(Cannot write register 0x%x, MSR_IA32_MISC_ENABLE);
        return err;
    }
    return err == 0;
}

int
_freqInit(void)
{
    int ret = 0;

    if (freq_init == NULL)
    {
#if defined(__x86_64__) || defined(__i386__)
        if (config.daemonMode == -1)
        {
            config.daemonMode = ACCESSMODE_DAEMON;
        }
        if (config.daemonMode == ACCESSMODE_DAEMON)
        {
            DEBUG_PLAIN_PRINT(DEBUGLEV_DEVELOP, Adjusting functions for daemon mode);
            freq_init = freq_init_client;
            freq_send = freq_send_client;
            freq_finalize = freq_finalize_client;
        }
        else if (config.daemonMode == ACCESSMODE_DIRECT)
        {
            DEBUG_PLAIN_PRINT(DEBUGLEV_DEVELOP, Adjusting functions for direct mode);
            freq_init = freq_init_direct;
            freq_send = freq_send_direct;
            freq_finalize = freq_finalize_direct;
        }
        else if (config.daemonMode == ACCESSMODE_PERF)
        {
            DEBUG_PLAIN_PRINT(DEBUGLEV_DEVELOP, Frequency module not usable in perf_event mode);
        }
#endif
        if (freq_init)
        {
            freq_init();
        }
        if (freq_init != freq_init_direct)
        {
            freq_init_direct();
        }
        freq_initialized = 1;
    }
}

void _freqFinalize(void)
{
    if (freq_finalize)
    {
        freq_finalize();
    }
    if (freq_finalize != freq_finalize_direct)
    {
        freq_finalize_direct();
    }
    freq_initialized = 0;
    freq_finalize = NULL;
    freq_send = NULL;
    freq_init = NULL;
    if (own_hpm)
        HPMfinalize();
}

uint64_t freq_setCpuClockMax(const int cpu_id, const uint64_t freq)
{
    char s[LIKWID_FREQUENCY_MAX_DATA_LENGTH];
    memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
    int ret = snprintf(s, LIKWID_FREQUENCY_MAX_DATA_LENGTH-1, "%lu", freq);
    if (!freq_initialized)
    {
        _freqInit();
    }
    if (ret > 0)
    {
        s[ret] = '\0';
        ret = freq_send(FREQ_WRITE, FREQ_LOC_MAX, cpu_id, ret, s);
        if (!ret)
        {
            return freq;
        }
    }
    return -EINVAL;
}

uint64_t freq_setCpuClockMin(const int cpu_id, const uint64_t freq)
{
    char s[LIKWID_FREQUENCY_MAX_DATA_LENGTH];
    memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
    int ret = snprintf(s, LIKWID_FREQUENCY_MAX_DATA_LENGTH-1, "%lu", freq);
    if (!freq_initialized)
    {
        _freqInit();
    }
    if (ret > 0)
    {
        s[ret] = '\0';
        ret = freq_send(FREQ_WRITE, FREQ_LOC_MIN, cpu_id, ret, s);
        if (!ret)
        {
            return freq;
        }
    }
    return 0;
}

uint64_t freq_setCpuClockCurrent(const int cpu_id, const uint64_t freq)
{
    char s[LIKWID_FREQUENCY_MAX_DATA_LENGTH];
    memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
    int ret = snprintf(s, LIKWID_FREQUENCY_MAX_DATA_LENGTH-1, "%lu", freq);
    if (!freq_initialized)
    {
        _freqInit();
    }
    if (ret > 0)
    {
        s[ret] = '\0';
        ret = freq_send(FREQ_WRITE, FREQ_LOC_CUR, cpu_id, ret, s);
        if (!ret)
        {
            return freq;
        }
    }
    return 0;
}


int freq_setGovernor(const int cpu_id, const char* gov)
{
    char s[LIKWID_FREQUENCY_MAX_DATA_LENGTH];
    memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
    int ret = snprintf(s, LIKWID_FREQUENCY_MAX_DATA_LENGTH-1, "%s", gov);
    if (!freq_initialized)
    {
        _freqInit();
    }
    if (ret > 0)
    {
        s[ret] = '\0';
        return freq_send(FREQ_WRITE, FREQ_LOC_GOV, cpu_id, LIKWID_FREQUENCY_MAX_DATA_LENGTH, s);
    }
    return -EINVAL;
}


uint64_t freq_getCpuClockCurrent(const int cpu_id)
{
    char s[LIKWID_FREQUENCY_MAX_DATA_LENGTH];
    if (!freq_initialized)
    {
        _freqInit();
    }
    memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
    int ret = freq_send_direct(FREQ_READ, FREQ_LOC_CUR, cpu_id, LIKWID_FREQUENCY_MAX_DATA_LENGTH, s);
    if (!ret)
    {
        uint64_t f = strtoull(s, NULL, 10);
        if (f > 0)
        {
            return f;
        }
    }
    return 0;
}

uint64_t freq_getCpuClockMin(const int cpu_id)
{
    char s[LIKWID_FREQUENCY_MAX_DATA_LENGTH];
    if (!freq_initialized)
    {
        _freqInit();
    }
    memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
    int ret = freq_send_direct(FREQ_READ, FREQ_LOC_MIN, cpu_id, LIKWID_FREQUENCY_MAX_DATA_LENGTH, s);
    if (!ret)
    {
        uint64_t f = strtoull(s, NULL, 10);
        if (f > 0)
        {
            return f;
        }
    }
    return -1;
}

uint64_t freq_getCpuClockMax(const int cpu_id)
{
    char s[LIKWID_FREQUENCY_MAX_DATA_LENGTH];
    if (!freq_initialized)
    {
        _freqInit();
    }
    memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
    int ret = freq_send_direct(FREQ_READ, FREQ_LOC_MAX, cpu_id, LIKWID_FREQUENCY_MAX_DATA_LENGTH, s);
    if (!ret)
    {
        uint64_t f = strtoull(s, NULL, 10);
        if (f > 0)
        {
            return f;
        }
    }
    return -1;
}

uint64_t freq_getConfCpuClockMin(const int cpu_id)
{
    char s[LIKWID_FREQUENCY_MAX_DATA_LENGTH];
    if (!freq_initialized)
    {
        _freqInit();
    }
    memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
    int ret = freq_send_direct(FREQ_READ, FREQ_LOC_CONF_MIN, cpu_id, LIKWID_FREQUENCY_MAX_DATA_LENGTH, s);
    if (!ret)
    {
        uint64_t f = strtoull(s, NULL, 10);
        if (f > 0)
        {
            return f;
        }
    }
    return -1;
}

uint64_t freq_getConfCpuClockMax(const int cpu_id)
{
    char s[LIKWID_FREQUENCY_MAX_DATA_LENGTH];
    if (!freq_initialized)
    {
        _freqInit();
    }
    memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
    int ret = freq_send_direct(FREQ_READ, FREQ_LOC_CONF_MAX, cpu_id, LIKWID_FREQUENCY_MAX_DATA_LENGTH, s);
    if (!ret)
    {
        uint64_t f = strtoull(s, NULL, 10);
        if (f > 0)
        {
            return f;
        }
    }
    return -1;
}

char * freq_getGovernor(const int cpu_id )
{
    if (!freq_initialized)
    {
        _freqInit();
    }
    char *s = malloc(LIKWID_FREQUENCY_MAX_DATA_LENGTH * sizeof(char));
    if (s)
    {
        memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
        int ret = freq_send_direct(FREQ_READ, FREQ_LOC_GOV, cpu_id, LIKWID_FREQUENCY_MAX_DATA_LENGTH, s);
        if (!ret)
        {
            s[strlen(s)-1] = '\0';
            return s;
        }
        free(s);
    }
    return NULL;
}

char * freq_getAvailFreq(const int cpu_id )
{
    if (!freq_initialized)
    {
        _freqInit();
    }
    char *s = malloc(LIKWID_FREQUENCY_MAX_DATA_LENGTH * sizeof(char));
    if (s)
    {
        memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
        int ret = freq_send_direct(FREQ_READ, FREQ_LOC_AVAIL_FREQ, cpu_id, LIKWID_FREQUENCY_MAX_DATA_LENGTH, s);
        if (!ret)
        {
            s[strlen(s)-1] = '\0';
            return s;
        }
        free(s);
    }
    return NULL;
}

char * freq_getAvailGovs(const int cpu_id )
{
    if (!freq_initialized)
    {
        _freqInit();
    }
    char *s = malloc(LIKWID_FREQUENCY_MAX_DATA_LENGTH * sizeof(char));
    if (s)
    {
        memset(s, '\0', LIKWID_FREQUENCY_MAX_DATA_LENGTH*sizeof(char));
        int ret = freq_send_direct(FREQ_READ, FREQ_LOC_AVAIL_GOV, cpu_id, LIKWID_FREQUENCY_MAX_DATA_LENGTH, s);
        if (!ret)
        {
            s[strlen(s)-1] = '\0';
            return s;
        }
        free(s);
    }
    return NULL;
}

int freq_getTurbo(const int cpu_id)
{
    if (isAMD())
        return getAMDTurbo(cpu_id);
    else
        return getIntelTurbo(cpu_id);
    return 1;
}

int freq_setTurbo(const int cpu_id, const int turbo)
{
    if (isAMD())
        return setAMDTurbo(cpu_id, turbo);
    else
        return setIntelTurbo(cpu_id, turbo);
    return 1;
}

void __attribute__((destructor (104))) close_frequency_cpu(void)
{
    _freqFinalize();
}
