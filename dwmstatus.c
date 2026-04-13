/*
 * Copy me if you can.
 * by 20h
 * Further modifications by Nour Nawar <nerdyforsciences@gmail.com>
 */

#define _BSD_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <X11/Xlib.h>
#include <alsa/asoundlib.h>

char *tzcairo = "Africa/Cairo";

static Display *dpy;

// simple prefix matcher
char*
guess(const char *path_prefix)
{
    char dir[512];
    char prefix[256];

    // Split into directory + prefix
    const char *last_slash = strrchr(path_prefix, '/');
    if (!last_slash)
		return NULL;

    size_t dir_len = last_slash - path_prefix;
    strncpy(dir, path_prefix, dir_len);
    dir[dir_len] = '\0';
    strcpy(prefix, last_slash + 1);

    DIR *d = opendir(dir);
    if (!d)
		return NULL;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            // allocate enough space for dir + "/" + filename + '\0'
            size_t path_len = strlen(dir) + 1 + strlen(entry->d_name) + 1;
            char *full_path = malloc(path_len);
            if (!full_path) {
                closedir(d);
                return NULL;
            }
            snprintf(full_path, path_len, "%s/%s", dir, entry->d_name);
            closedir(d);
            return full_path;  // caller must free
        }
    }
    closedir(d);
    return NULL;
}

// get the only one subdirectory under a base directory
char*
get_that_one_subdir(const char* base)
{
	DIR *d = opendir(base);
	if(d == NULL)
		return NULL;
	struct dirent* entry;
	while((entry = readdir(d)) != NULL){
		if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", base, entry->d_name);

	struct stat st;
	stat(path, &st);
	if(S_ISDIR(st.st_mode)){
		closedir(d);
		return strdup(path);
	}
}
	closedir(d);
	return NULL;
}

char *
smprintf(char *fmt, ...)
{
	va_list fmtargs;
	char *ret;
	int len;

	va_start(fmtargs, fmt);
	len = vsnprintf(NULL, 0, fmt, fmtargs);
	va_end(fmtargs);

	ret = malloc(++len);
	if (ret == NULL) {
		perror("malloc");
		exit(1);
	}

	va_start(fmtargs, fmt);
	vsnprintf(ret, len, fmt, fmtargs);
	va_end(fmtargs);

	return ret;
}

void
settz(char *tzname)
{
	setenv("TZ", tzname, 1);
}

char *
mktimes(char *fmt, char *tzname)
{
	char buf[129];
	time_t tim;
	struct tm *timtm;

	settz(tzname);
	tim = time(NULL);
	timtm = localtime(&tim);
	if (timtm == NULL)
		return smprintf("");

	if (!strftime(buf, sizeof(buf)-1, fmt, timtm)) {
		fprintf(stderr, "strftime == 0\n");
		return smprintf("");
	}

	return smprintf("%s", buf);
}

void
setstatus(char *str)
{
	XStoreName(dpy, DefaultRootWindow(dpy), str);
	XSync(dpy, False);
}

char *
loadavg(void)
{
	double avgs[3];

	if (getloadavg(avgs, 3) < 0)
		return smprintf("");

	return smprintf("%.2f %.2f %.2f", avgs[0], avgs[1], avgs[2]);
}

char *
readfile(char *base, char *file)
{
	char *path, line[513];
	FILE *fd;

	memset(line, 0, sizeof(line));

	path = smprintf("%s/%s", base, file);
	fd = fopen(path, "r");
	free(path);
	if (fd == NULL)
		return NULL;

	if (fgets(line, sizeof(line)-1, fd) == NULL) {
		fclose(fd);
		return NULL;
	}
	fclose(fd);

	return smprintf("%s", line);
}

char *
getmem(void)
{
	long total, available;
	long used;
	const char* path = "/proc/meminfo";
	char line[64];
	FILE* fd;
	fd = fopen(path, "r");
	while(fgets(line, sizeof(line), fd)){
		if(sscanf(line,"MemTotal: %ld kB", &total)== 1)
			continue;
		if(sscanf(line,"MemAvailable: %ld kB", &available)== 1)
			break;
	}
	fclose(fd);
	used = total - available;
	return smprintf("%0.2f GiB", (float)(used)/(1024.0 * 1024.0));
}
char *
getbattery(void)
{
	char *co, status;
	int descap = -1;
	int remcap = -1;
	char* path;

	path = guess("/sys/class/power_supply/BAT");
	if(path == NULL)
		return smprintf("");
	co = readfile(path, "present");
	if (co == NULL){
		free(path);
		return smprintf("");
	}
	if (co[0] != '1') {
		free(co);
		return smprintf("not present");
	}
	free(co);

	co = readfile(path, "energy_full_design");
	if (co == NULL){
		free(path);
		return smprintf("");
	}
	sscanf(co, "%d", &descap);
	free(co);

	co = readfile(path, "energy_now");
	if (co == NULL){
		free(path);
		return smprintf("");
	}
	sscanf(co, "%d", &remcap);
	free(co);

	co = readfile(path, "status");
	if (!strncmp(co, "Discharging", 11)) {
		status = '-';
	} else if(!strncmp(co, "Charging", 8)) {
		status = '+';
	} else if(!strncmp(co, "Not charging", 12)) {
		status = '=';
	}else {
		status = '?';
	}
	free(co);
	free(path);

	if (remcap < 0 || descap < 0)
		return smprintf("invalid");
	return smprintf("%.0f%%%c", ((float)remcap / (float)descap) * 100, status);
}

char *
gettemperature(char *base, char *sensor)
{
	char *co;
	char *ret;

	co = readfile(base, sensor);
	if (co == NULL)
		return smprintf("");

	ret = smprintf("%02.0f°C", atof(co) / 1000);
	free(co);
	return ret;
}
char*
getrxtxstats(char* path)
{
	double rx_bytes, tx_bytes;
	char* rxo, *txo, *ret;
	char* rx_buff = readfile(path, "statistics/rx_bytes");
	char* tx_buff = readfile(path, "statistics/tx_bytes");
	if(!rx_buff || !tx_buff){
		free(rx_buff); free(tx_buff);
		return smprintf("?");
	}
	sscanf(rx_buff, "%lf", &rx_bytes);
	sscanf(tx_buff, "%lf", &tx_bytes);
	if(rx_bytes >= 1024 * 1024 * 1024)
		rxo = smprintf("%1.2fG↓", rx_bytes / (1024.0 * 1024.0 * 1024.0));
	else if(rx_bytes >= 1024 * 1024)
		rxo = smprintf("%1.0fM↓", rx_bytes / (1024.0 * 1024.0));
	else if(rx_bytes >= 1024)
		rxo = smprintf("%1.0fK↓", rx_bytes / 1024.0);
	else
		rxo = smprintf("%1.0fB↓", rx_bytes);

	if(tx_bytes >= 1024 * 1024 * 1024)
		txo = smprintf("%1.2fG↑", tx_bytes / (1024.0 * 1024.0 * 1024.0));
	else if(tx_bytes >= 1024 * 1024)
		txo = smprintf("%1.0fM↑", tx_bytes / (1024.0 * 1024.0));
	else if(tx_bytes >= 1024)
		txo = smprintf("%1.0fK↑", tx_bytes / 1024.0);
	else
		txo = smprintf("%1.0fB↑", tx_bytes);

    ret = smprintf("%s/%s", rxo, txo);
	free(tx_buff); free(rx_buff);
	free(rxo); free(txo);
	return ret;
}

char*
getwlan(void)
{
	char* guessed = guess("/sys/class/net/wlp");
    char *co = readfile(guessed, "operstate");
	char *ret;
    if (co == NULL){
		free(guessed);
        return smprintf("?");
	}
    for (int i = 0; co[i]; i++) {
        if (co[i] == '\n') co[i] = '\0';
    }
	if(strncmp(co, "up",2) == 0){
		ret = getrxtxstats(guessed);
	}
	else{
		ret = smprintf("%s", co);
	}
	free(guessed);
	free(co);
	return ret;
}

char*
getwired(void)
{
	char* guessed = guess("/sys/class/net/enp");
    char *co = readfile(guessed, "operstate");
	char* ret;
    if (co == NULL){
		free(guessed);
        return smprintf("?");
	}
    for (int i = 0; co[i]; i++) {
        if (co[i] == '\n') co[i] = '\0';
    }
	if(strncmp(co, "up",2) == 0){
		ret = getrxtxstats(guessed);
	}
	else{
		ret = smprintf("%s", co);
	}
	free(guessed);
	free(co);
	return ret;
}

char*
getbright(void)
{
	int max;
	int current;
	char* base = "/sys/class/backlight/";
	char* full_base = get_that_one_subdir((const char*) base);
    char *co = readfile(full_base, "max_brightness");
    if (co == NULL){
		free(full_base);
        return smprintf("?");
	}
	sscanf(co, "%d", &max);
	free(co);
    co = readfile(full_base, "brightness");
    if (co == NULL){
		free(full_base);
        return smprintf("?");
	}
	sscanf(co, "%d", &current);

	free(full_base);
	free(co);
	return smprintf("%.0f%%", ((float)current / (float)max) * 100);
}

char *
getcpu(void)
{
    FILE *fp;
    unsigned long long int user1, nice1, system1, idle1, iowait1, irq1, softirq1, steal1;
    unsigned long long int user2, nice2, system2, idle2, iowait2, irq2, softirq2, steal2;
    unsigned long long int total1, total2, total_diff, idle_diff;
    int usage = 0;

    // read first snapshot
    fp = fopen("/proc/stat","r");
    if (!fp)
		return smprintf("?");
    fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &user1, &nice1, &system1, &idle1, &iowait1, &irq1, &softirq1, &steal1);
    fclose(fp);

	// sleep 100 ms
    usleep(100000);

    // read second snapshot
    fp = fopen("/proc/stat", "r");
    if (!fp)
		return smprintf("?");
    fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &user2, &nice2, &system2, &idle2, &iowait2, &irq2, &softirq2, &steal2);
    fclose(fp);

    total1 = user1 + nice1 + system1 + idle1 + iowait1 + irq1 + softirq1 + steal1;
    total2 = user2 + nice2 + system2 + idle2 + iowait2 + irq2 + softirq2 + steal2;

    total_diff = total2 - total1;
    idle_diff  = (idle2 + iowait2) - (idle1 + iowait1);

    if (total_diff == 0)
		return smprintf("?");

    usage = (int)((total_diff - idle_diff) * 100 / total_diff);

	return smprintf("%.0f%%", (float)usage);
}

// Use mic for sink
// Use speaker for source
char *
getsound(const char *device)
{
    snd_mixer_t *mixer = NULL;
    snd_mixer_selem_id_t *sid = NULL;
    snd_mixer_elem_t *elem = NULL;
    long minv, maxv, value;
    int muted = 0;
    int mic = 0;
    const char *selem_name = NULL;

    if (strcmp(device, "mic") == 0) {
        selem_name = "Capture";
        mic = 1;
    } else if (strcmp(device, "speaker") == 0) {
        selem_name = "Master";
    } else {
        return smprintf("?");
    }

    if (snd_mixer_open(&mixer, 0) < 0)
        return smprintf("?");
    if (snd_mixer_attach(mixer, "default") < 0)
        return smprintf("?");

    snd_mixer_selem_register(mixer, NULL, NULL);
    snd_mixer_load(mixer);

    snd_mixer_selem_id_malloc(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);

    elem = snd_mixer_find_selem(mixer, sid);
    if (!elem) {
        snd_mixer_selem_id_free(sid);
        snd_mixer_close(mixer);
        return smprintf("?");
    }
    if (mic) {
        snd_mixer_selem_get_capture_volume_range(elem, &minv, &maxv);
        snd_mixer_selem_get_capture_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &value);
        snd_mixer_selem_get_capture_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &muted);
        muted = !muted;
    } else {
        snd_mixer_selem_get_playback_volume_range(elem, &minv, &maxv);
        snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &value);
        snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &muted);
        muted = !muted;
    }

    // cleanup
    snd_mixer_selem_id_free(sid);
    snd_mixer_close(mixer);

    if (muted)
        return smprintf("MUTE");

    int pct = (int)((value - minv) * 100 / (maxv - minv));
    return smprintf("%d%%", pct);
}

char *
execscript(char *cmd)
{
	FILE *fp;
	char retval[1025], *rv;

	memset(retval, 0, sizeof(retval));

	fp = popen(cmd, "r");
	if (fp == NULL)
		return smprintf("");

	rv = fgets(retval, sizeof(retval), fp);
	pclose(fp);
	if (rv == NULL)
		return smprintf("");
	retval[strlen(retval)-1] = '\0';

	return smprintf("%s", retval);
}

int
main(void)
{
	char *status;
	char *bat;
	char *tmcairo;
	char *t1;
	char *kbmap;
	char *wlan;
	char *eth;
	char *cpu;
	char *vol;
	char *mic;
	char *bright;
	char *mem;

	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "dwmstatus: cannot open display.\n");
		return 1;
	}

	// Update every second
	for (;;sleep(1)) {
		// very not portable code!
		bat = getbattery();
		tmcairo= mktimes("%H:%M:%S", tzcairo);
		kbmap = execscript("setxkbmap -query | grep layout | cut -d':' -f 2- | tr -d ' '");
		t1 = gettemperature("/sys/class/hwmon/hwmon6", "temp1_input");

		eth = getwired();
		wlan = getwlan();
		cpu = getcpu();
		vol = getsound("speaker");
		mic = getsound("mic");
		bright = getbright();
		mem = getmem();

		status = smprintf("NN | K:%s | CPU:%s | U:%s | wlan:%s | eth:%s | Mic:%s | Vol:%s | Bri:%s | T:%s | B:%s | %s ",
				kbmap, cpu, mem, wlan, eth, mic, vol, bright, t1, bat, tmcairo);
		setstatus(status);

		free(status);
		free(bat);
		free(tmcairo);
		free(t1);
		free(kbmap);
		free(wlan);
		free(eth);
		free(cpu);
		free(vol);
		free(mic);
		free(bright);
		free(mem);
	}

	XCloseDisplay(dpy);

	return 0;
}

