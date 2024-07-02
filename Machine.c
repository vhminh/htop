/*
htop - Machine.c
(C) 2023 Red Hat, Inc.
(C) 2004,2005 Hisham H. Muhammad
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "Machine.h"

#include <stdlib.h>
#include <unistd.h>

#include "Object.h"
#include "Platform.h"
#include "Row.h"
#include "XUtils.h"


void Machine_init(Machine* this, UsersTable* usersTable, uid_t userId) {
   this->usersTable = usersTable;
   this->userId = userId;

   this->htopUserId = getuid();

   // discover fixed column width limits
   Row_setPidColumnWidth(Platform_getMaxPid());

   // always maintain valid realtime timestamps
   Platform_gettime_realtime(&this->realtime, &this->realtimeMs);

#ifdef HAVE_LIBHWLOC
   this->topologyOk = false;
   if (hwloc_topology_init(&this->topology) == 0) {
      this->topologyOk =
         #if HWLOC_API_VERSION < 0x00020000
         /* try to ignore the top-level machine object type */
         0 == hwloc_topology_ignore_type_keep_structure(this->topology, HWLOC_OBJ_MACHINE) &&
         /* ignore caches, which don't add structure */
         0 == hwloc_topology_ignore_type_keep_structure(this->topology, HWLOC_OBJ_CORE) &&
         0 == hwloc_topology_ignore_type_keep_structure(this->topology, HWLOC_OBJ_CACHE) &&
         0 == hwloc_topology_set_flags(this->topology, HWLOC_TOPOLOGY_FLAG_WHOLE_SYSTEM) &&
         #else
         0 == hwloc_topology_set_all_types_filter(this->topology, HWLOC_TYPE_FILTER_KEEP_STRUCTURE) &&
         #endif
         0 == hwloc_topology_load(this->topology);
   }
#endif
}

void Machine_done(Machine* this) {
#ifdef HAVE_LIBHWLOC
   if (this->topologyOk) {
      hwloc_topology_destroy(this->topology);
   }
#endif
   Object_delete(this->processTable);
   free(this->tables);
}

static void Machine_addTable(Machine* this, Table* table) {
   /* check that this table has not been seen previously */
   for (size_t i = 0; i < this->tableCount; i++)
      if (this->tables[i] == table)
         return;

   size_t nmemb = this->tableCount + 1;
   Table** tables = xReallocArray(this->tables, nmemb, sizeof(Table*));
   tables[nmemb - 1] = table;
   this->tables = tables;
   this->tableCount++;
}

void Machine_populateTablesFromSettings(Machine* this, Settings* settings, Table* processTable) {
   this->settings = settings;
   this->processTable = processTable;

   for (size_t i = 0; i < settings->nScreens; i++) {
      ScreenSettings* ss = settings->screens[i];
      Table* table = ss->table;
      if (!table)
         table = ss->table = processTable;
      if (i == 0)
         this->activeTable = table;

      Machine_addTable(this, table);
   }
}

void Machine_setTablesPanel(Machine* this, Panel* panel) {
   for (size_t i = 0; i < this->tableCount; i++) {
      Table_setPanel(this->tables[i], panel);
   }
}

static long Machine_populateAccForProcess(Vector* rows, int n, int cur) {
   Row* row = (Row*) Vector_get(rows, cur);
   Process* process = (Process*) row;
   if (process->m_accResident != -1) {
      return process->m_accResident;
   }

   process->m_accResident = process->m_resident;
   // FIXME: this very inefficient because it runs in O(n^2) but I don't know how to create a HashMap<int, Vector<int>>
   for (int i = 0; i < n; i++) {
      if (i == cur) continue;
      Row* r = (Row*) Vector_get(rows, i);
      if (r->parent == row->id) {
         process->m_accResident += Machine_populateAccForProcess(rows, n, i);
      }
   }
   return process->m_accResident;
}

static void Machine_populateAccumulatedFields(Machine* this) {
   int n = Vector_size(this->processTable->rows);

   // initialize to -1
   for (int i = 0; i < n; i++) {
      Process* process = (Process*) Vector_get(this->processTable->rows, i);
      process->m_accResident = -1;
   }

   for (int i = 0; i < n; i++) {
      Machine_populateAccForProcess(this->processTable->rows, n, i);
   }

   for (int i = 0; i < n; i++) {
      // FIXME: a hack to temporary display accumulated rss for testing
      Process* process = (Process*) Vector_get(this->processTable->rows, i);
      process->m_resident = process->m_accResident;
   }
}

void Machine_scanTables(Machine* this) {
   // set scan timestamp
   static bool firstScanDone = false;

   if (firstScanDone) {
      this->prevMonotonicMs = this->monotonicMs;
      Platform_gettime_monotonic(&this->monotonicMs);
   } else {
      this->prevMonotonicMs = 0;
      this->monotonicMs = 1;
      firstScanDone = true;
   }
   assert(this->monotonicMs > this->prevMonotonicMs);

   this->maxUserId = 0;
   Row_resetFieldWidths();

   for (size_t i = 0; i < this->tableCount; i++) {
      Table* table = this->tables[i];

      // pre-processing of each row
      Table_scanPrepare(table);

      // scan values for this table
      Table_scanIterate(table);

      // post-process after scanning
      Table_scanCleanup(table);
   }

   Machine_populateAccumulatedFields(this);

   Row_setUidColumnWidth(this->maxUserId);
}
