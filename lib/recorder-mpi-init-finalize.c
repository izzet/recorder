/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright 2012 by The HDF Group
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 * 4. All publications or advertising materials mentioning features or use of
 *    this software are asked, but not required, to acknowledge that it was
 *    developed by The HDF Group and by the National Center for Supercomputing
 *    Applications at the University of Illinois at Urbana-Champaign and
 *    credit the contributors.
 *
 * 5. Neither the name of The HDF Group, the name of the University, nor the
 *    name of any Contributor may be used to endorse or promote products derived
 *    from this software without specific prior written permission from
 *    The HDF Group, the University, or the Contributor, respectively.
 *
 * DISCLAIMER:
 * THIS SOFTWARE IS PROVIDED BY THE HDF GROUP AND THE CONTRIBUTORS
 * "AS IS" WITH NO WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED. In no
 * event shall The HDF Group or the Contributors be liable for any damages
 * suffered by the users arising out of the use of this software, even if
 * advised of the possibility of such damage.
 *
 * Portions of Recorder were developed with support from the Lawrence Berkeley
 * National Laboratory (LBNL) and the United States Department of Energy under
 * Prime Contract No. DE-AC02-05CH11231.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE /* for RTLD_NEXT */

#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>
#include <string.h>

#include "mpi.h"
#include "recorder.h"

char *comm2name(MPI_Comm comm) {
    char *tmp = (char *)malloc(1024);
    int len;
    PMPI_Comm_get_name(comm, tmp, &len);
    tmp[len] = 0;
    if(len == 0) strcpy(tmp, "MPI_COMM_UNKNOWN");
    return tmp;
}

char *type2name(MPI_Datatype type) {
    char *tmp = (char *)malloc(1024);
    int len;
    PMPI_Type_get_name(type, tmp, &len);
    tmp[len] = 0;
    return tmp;
}

char *makename(MPI_Datatype *type) {
    char *tmp = (char *)malloc(1024);
    sprintf(tmp, "RECORDER_TYPE_%p", type);
    return tmp;
}
