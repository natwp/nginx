'use strict';

const childProcess = require('child_process');
const os = require('os');

const wmic = process.env.WMIC_PATH
  || 'C:\\Windows\\System32\\wbem\\WMIC.exe';
const processTimes = process.env.PROCESS_TIMES_PATH
  || 'win32-process-times.exe';
const logicalProcessors = Math.max(1, os.cpus().length);

function runWmic(args) {
  if (process.platform !== 'win32') {
    return '';
  }

  try {
    return childProcess.execFileSync(wmic, args, {
      encoding: 'utf8',
      maxBuffer: 8 * 1024 * 1024,
      windowsHide: true
    });
  } catch (error) {
    return '';
  }
}

function csvRows(text) {
  const lines = text.replace(/\r/g, '').split('\n')
    .map(line => line.trim())
    .filter(Boolean);

  if (!lines.length) {
    return [];
  }

  const header = lines[0].split(',');
  const rows = [];

  for (const line of lines.slice(1)) {
    const fields = line.split(',');
    if (fields.length !== header.length) {
      continue;
    }

    const row = {};
    header.forEach((name, index) => {
      row[name] = fields[index];
    });
    rows.push(row);
  }

  return rows;
}

function normalize(value) {
  return String(value || '').replace(/\\/g, '/').toLowerCase();
}

function findProcesses(prefix) {
  if (process.platform !== 'win32' || !prefix) {
    return [];
  }

  const wanted = normalize(prefix);
  const rows = csvRows(runWmic([
    'process', 'get',
    'CommandLine,Name,ParentProcessId,ProcessId', '/format:csv'
  ]));
  const processes = [];

  for (const row of rows) {
    const name = String(row.Name || '').toLowerCase();
    const commandLine = normalize(row.CommandLine);
    const pid = Number(row.ProcessId);
    const parentPid = Number(row.ParentProcessId);

    if (name === 'nginx.exe' && commandLine.includes(wanted)
        && Number.isInteger(pid) && pid > 0
        && !processes.some(process => process.pid === pid)) {
      processes.push({
        pid,
        parentPid: Number.isInteger(parentPid) ? parentPid : 0,
        commandLine
      });
    }
  }

  const pids = new Set(processes.map(process => process.pid));
  const roots = processes.filter(process => !pids.has(process.parentPid));

  for (const process of processes) {
    if (processes.length === 1) {
      process.role = 'single';
    } else if (roots.some(root => root.pid === process.pid)) {
      process.role = 'master-router';
    } else {
      process.role = 'worker';
    }
  }

  return processes;
}

function findPids(prefix) {
  return findProcesses(prefix).map(process => process.pid);
}

function readCounters(pids) {
  const counters = new Map();

  if (process.platform !== 'win32' || !pids.length) {
    return counters;
  }

  try {
    const output = childProcess.execFileSync(
      processTimes, pids.map(String), {
        encoding: 'utf8',
        maxBuffer: 1024 * 1024,
        windowsHide: true
      }
    );

    for (const line of output.replace(/\r/g, '').split('\n')) {
      const fields = line.trim().split(/\s+/).map(Number);
      if (fields.length !== 3 || !fields.every(Number.isFinite)) {
        continue;
      }

      counters.set(fields[0], fields[1] + fields[2]);
    }

    if (counters.size) {
      return counters;
    }
  } catch (error) {
    // Fall back to WMI when the optional native helper is unavailable.
  }

  const rows = csvRows(runWmic([
    'process', 'get', 'KernelModeTime,ProcessId,UserModeTime',
    '/format:csv'
  ]));

  for (const row of rows) {
    const pid = Number(row.ProcessId);
    const kernel = Number(row.KernelModeTime);
    const user = Number(row.UserModeTime);

    if (pids.includes(pid) && Number.isFinite(kernel)
        && Number.isFinite(user)) {
      counters.set(pid, kernel + user);
    }
  }

  return counters;
}

function snapshot(pids) {
  return {
    at: performance.now(),
    counters: readCounters(pids)
  };
}

function utilization(start, end, pidCount, pids) {
  if (!start || !end || process.platform !== 'win32') {
    return {
      nginxCpuPercent: null,
      nginxCpuCores: null,
      nginxCpuPidCount: pidCount || 0
    };
  }

  const elapsed = (end.at - start.at) / 1000;
  if (!(elapsed > 0)) {
    return {
      nginxCpuPercent: null,
      nginxCpuCores: null,
      nginxCpuPidCount: pidCount || 0
    };
  }

  let ticks = 0;
  for (const [pid, value] of end.counters) {
    if (pids && !pids.includes(pid)) {
      continue;
    }

    const before = start.counters.get(pid);
    if (before !== undefined && value >= before) {
      ticks += value - before;
    }
  }

  const cpuCores = ticks / 10000000 / elapsed;

  return {
    nginxCpuPercent: cpuCores / logicalProcessors * 100,
    nginxCpuCores: cpuCores,
    nginxCpuPidCount: pidCount || 0
  };
}

function utilizationByRole(start, end, processes) {
  const pids = processes.map(process => process.pid);
  const result = utilization(start, end, processes.length, pids);

  if (!start || !end || process.platform !== 'win32') {
    return {...result, nginxCpuByRole: null};
  }

  const elapsed = (end.at - start.at) / 1000;
  if (!(elapsed > 0)) {
    return {...result, nginxCpuByRole: null};
  }

  const ticksByRole = new Map();

  for (const process of processes) {
    const before = start.counters.get(process.pid);
    const after = end.counters.get(process.pid);

    if (before === undefined || after === undefined || after < before) {
      continue;
    }

    const role = process.role || 'unknown';
    ticksByRole.set(role, (ticksByRole.get(role) || 0) + after - before);
  }

  const nginxCpuByRole = {};
  for (const [role, ticks] of ticksByRole) {
    const cpuCores = ticks / 10000000 / elapsed;
    nginxCpuByRole[role] = {
      cpuCores,
      cpuPercent: cpuCores / logicalProcessors * 100
    };
  }

  return {...result, nginxCpuByRole};
}

function clientSnapshot() {
  return process.cpuUsage();
}

function clientUtilization(start, elapsed) {
  if (!start || !(elapsed > 0)) {
    return {
      clientCpuPercent: null,
      clientCpuCores: null
    };
  }

  const usage = process.cpuUsage(start);
  const cpuCores = (usage.user + usage.system) / 1000000 / elapsed;

  return {
    clientCpuPercent: cpuCores / logicalProcessors * 100,
    clientCpuCores: cpuCores
  };
}

module.exports = {
  clientSnapshot,
  clientUtilization,
  findProcesses,
  findPids,
  snapshot,
  utilization,
  utilizationByRole
};
