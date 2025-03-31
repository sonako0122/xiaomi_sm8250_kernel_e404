#ifndef _E404_ATTRIBUTES_H
#define _E404_ATTRIBUTES_H

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/cpufreq.h>

struct e404_attributes {
    int e404_kernelsu;
    int e404_rom_type;
    int e404_effcpu;
    int e404_cpu_input_boost;
    int e404_dvq_input_boost;
    int e404_panel_width;
    int e404_panel_height;
    int e404_oem_panel_width;
    int e404_oem_panel_height;
};

extern struct e404_attributes e404_data;

static inline int extract_e404_cmdline(const char *cmdline, const char *key, u32 *value) {
    char *ptr = strstr(cmdline, key);
    char temp[16];
    int ret;

    if (!ptr)
        return -1;

    ptr += strlen(key);
    sscanf(ptr, "%15s", temp);
    ret = kstrtouint(temp, 10, value);
    
    if (ret) {
        pr_alert("E404: Failed to parse %s from cmdline! Error %d\n", key, ret);
        return ret;
    }

    pr_alert("E404: %s parsed as %d\n", key, *value);
    return 0;
}

#endif /* _E404_ATTRIBUTES_H */
