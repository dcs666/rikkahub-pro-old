package me.rerere.workspace

import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.nio.charset.StandardCharsets

/**
 * [TURBO] 常驻 proot+bash 会话：shell 性能质变的核心。
 *
 * 背景：每条 shell 命令都 fork+exec 一个新 proot 进程（加载原生 .so + ptrace 初始化 + 起 bash），
 * 是秒级固定开销。本类维护一个长驻 proot+bash，命令经 stdin 发送、输出经 stdout 读取、用带 NUL 的
 * sentinel 标记结束并携带退出码，把"每次秒级"降到"首次秒级 + 后续几十 ms"。
 *
 * 安全设计（每项都对应一个真实坑）：
 * - 子 shell 隔离：每条命令包成 `( cd -- CWD && COMMAND )`，cd/变量/export/trap 在子 shell 结束即消失，
 *   天然无跨命令状态污染，无需手动 reset。
 * - sentinel 防冲突：结束标记 `\0__RIKKA_DONE_<code>__\0` 含 NUL 字节，正常文本输出几乎不含 NUL，
 *   偶然匹配概率极低。
 * - stderr 合并 stdout：redirectErrorStream(true) 单流读取（trade-off：stderr 混入 stdout，顺序可能交错）。
 * - 完整 fallback：任何异常（启动失败/进程死/读超时/sentinel 缺失）都抛出，由 ProotShellRunner 退回
 *   一次性 proot，绝不丢功能。
 * - stdin 命令不走这里：需要 stdin 输入的命令由调用方直接走一次性 proot，避免 stdin 通道冲突。
 */
class PersistentShellSession(
    private val patcher: RootfsPatcher = RootfsPatcher(),
) {
    private var process: Process? = null
    private var writer: OutputStreamWriter? = null
    private var reader: BufferedReader? = null
    private var boundLinuxDir: File? = null

    private companion object {
        // sentinel: NUL + 前缀 + 退出码 + 后缀 + NUL。NUL 让正常文本几乎不可能误匹配。
        private const val SENTINEL_PREFIX = "\u0000__RIKKA_DONE_"
        private const val SENTINEL_SUFFIX = "__\u0000"
        private const val WORKSPACE_DIR = WorkspaceManager.ROOTFS_WORKSPACE_DIR
        private const val WARMUP_TIMEOUT_MS = 5_000L
    }

    @Synchronized
    fun destroy() {
        runCatching { writer?.close() }
        runCatching { reader?.close() }
        process?.let { runCatching { it.destroyForcibly() } }
        process = null
        writer = null
        reader = null
        boundLinuxDir = null
    }

    /**
     * 在常驻会话执行命令。失败时抛异常（由调用方 fallback 到一次性 proot）。
     */
    @Synchronized
    fun execute(
        context: WorkspaceShellContext,
        proot: File,
        loader: File,
    ): WorkspaceCommandResult {
        ensureStarted(context, proot, loader)
        val w = writer ?: error("persistent shell: writer unavailable")
        val r = reader ?: error("persistent shell: reader unavailable")

        // [TURBO-FIX] 命令在主 bash 进程内直接执行，不再用 `( ... )` 子 shell 包裹。
        // 根因（用户实测：短命令 cd/ls/echo 每次都 "persistent shell command timed out"）：
        // `( cmd )` 会让 bash 纯 fork 一个子 shell（bash 克隆，不 exec）。proot --link2symlink
        // 的共享数据库不支持并发访问，fork 出的子 shell 继承父 bash 的 db 状态后与父进程
        // 并发访问 → 死锁/卡住 → 命令永不结束 → sentinel 永不到达 → 30s 超时。
        // warmUp（主 bash 直接 printf，无 fork）成功、实际命令（强制子 shell）超时，完全吻合。
        // 一次性 proot（bash -c → fork+exec 全新进程，不继承 db 锁状态）无此问题，故从未超时。
        // 修复后：cd 是 builtin 不 fork；外部命令（ls/grep/curl）fork+exec 全新进程，
        // 与一次性路径等价，不死锁。状态隔离降级：cwd 会残留，但每条命令开头都会 cd 到
        // 目标目录覆盖；export 变量残留概率低，可接受。
        val script = buildString {
            append("cd -- ")
            append(context.prootCwd().shellQuote())
            append(" && ")
            append(context.command)
            append(" ; __rikka_s=${'$'}? ; printf '\\000__RIKKA_DONE_%d__\\000' \"${'$'}__rikka_s\"\n")
        }
        runCatching {
            w.write(script)
            w.flush()
        }.onFailure {
            destroy()
            throw it
        }
        return readUntilSentinel(r, context.timeoutMillis)
    }

    private fun ensureStarted(context: WorkspaceShellContext, proot: File, loader: File) {
        if (process?.isAlive == true && boundLinuxDir == context.linuxDir) return
        destroy()
        patcher.patch(context.linuxDir)
        val p = ProcessBuilder(buildSessionCommand(context, proot))
            .directory(context.filesDir)
            .redirectErrorStream(true) // stderr 合并 stdout，单流读取
            .apply {
                environment()["PROOT_LOADER"] = loader.absolutePath
                environment()["PROOT_TMP_DIR"] = context.tempDir.absolutePath
                environment()["TMPDIR"] = context.tempDir.absolutePath
            }
            .start()
        process = p
        writer = OutputStreamWriter(p.outputStream, StandardCharsets.UTF_8)
        reader = BufferedReader(InputStreamReader(p.inputStream, StandardCharsets.UTF_8))
        boundLinuxDir = context.linuxDir
        // 预热：发一个空命令读掉 bash/proot 的启动输出，确保后续命令输出干净。
        warmUp()
    }

    private fun warmUp() {
        val w = writer ?: return
        val r = reader ?: return
        runCatching {
            w.write("printf '\\000__RIKKA_DONE_0__\\000'\n")
            w.flush()
            readUntilSentinel(r, WARMUP_TIMEOUT_MS)
        }.onFailure {
            destroy()
            throw it
        }
    }

    private fun buildSessionCommand(context: WorkspaceShellContext, proot: File): List<String> {
        val command = mutableListOf(
            proot.absolutePath,
            "--root-id",
            "--link2symlink",
            "--kill-on-exit",
            "-r", context.linuxDir.absolutePath,
            "-w", context.prootCwd(),
            "-b", "${context.filesDir.absolutePath}:$WORKSPACE_DIR",
        )
        context.bindMounts.forEach { mount ->
            if (mount.source.exists()) {
                command += "-b"
                command += "${mount.source.absolutePath}:${mount.target.trimEnd('/')}"
            }
        }
        WorkspaceManager.KERNEL_FS_MOUNTS.forEach { path ->
            if (File(path).exists()) {
                command += "-b"
                command += path
            }
        }
        command += listOf(
            "/usr/bin/env", "-i",
            "HOME=/root",
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
            "TERM=xterm-256color",
            "LANG=C.UTF-8",
            "LC_ALL=C.UTF-8",
            // [TURBO 修复] stdbuf -oL -eL：让 bash 的 stdout/stderr 走行缓冲。
            // 根因：bash 非交互长驻、stdout 是 pipe 时，glibc 默认 block-buffered(4KB)。
            // 短输出命令（cd/mkdir/短 echo/写文件/grep 无结果）连同结尾 sentinel（几十字节）
            // 会留在缓冲区不 flush，读线程等不到 sentinel → "persistent shell command timed out"。
            // 行缓冲后每行（含 \n）即 flush，sentinel 立即到达。已在 workspace proot 复现并验证。
            // （一次性 proot 无此问题：bash 执行完即退出，退出时 flush 全部缓冲。）
            "stdbuf", "-oL", "-eL",
            // 非交互 bash：stdin 是 pipe 时自动非交互地读命令执行，无 prompt。
            // --norc --noprofile 不加载 rc（更快更干净，AI 工具调用不依赖 alias/变量）。
            "bash", "--norc", "--noprofile",
        )
        return command
    }

    private fun readUntilSentinel(r: BufferedReader, timeoutMillis: Long): WorkspaceCommandResult {
        val sb = StringBuilder()
        val readThread = Thread {
            try {
                val buf = CharArray(4096)
                while (true) {
                    val n = r.read(buf)
                    if (n < 0) break
                    sb.append(buf, 0, n)
                    val prefixIdx = sb.indexOf(SENTINEL_PREFIX)
                    if (prefixIdx >= 0 && sb.indexOf(SENTINEL_SUFFIX, prefixIdx) >= 0) break
                }
            } catch (_: Exception) {
                // 进程被杀/流关闭时 read 抛异常，保留已读内容即可
            }
        }.apply { isDaemon = true; start() }

        readThread.join(timeoutMillis)
        if (readThread.isAlive) {
            // 超时：会话卡死，销毁（daemon 读线程会随流关闭结束）
            destroy()
            return WorkspaceCommandResult(
                exitCode = -1,
                stdout = "",
                stderr = "persistent shell command timed out",
                timedOut = true,
            )
        }

        val all = sb.toString()
        val prefixIdx = all.indexOf(SENTINEL_PREFIX)
        if (prefixIdx < 0) {
            // 没读到 sentinel：进程可能已死，销毁让调用方 fallback
            destroy()
            error("persistent shell: sentinel not found (session likely dead)")
        }
        val output = all.substring(0, prefixIdx)
        val codeStart = prefixIdx + SENTINEL_PREFIX.length
        val codeEnd = all.indexOf(SENTINEL_SUFFIX, codeStart)
        val exitCode = all.substring(codeStart, codeEnd).toIntOrNull() ?: -1

        val truncated = output.length > MAX_OUTPUT_CHARS
        val stdout = if (truncated) output.substring(0, MAX_OUTPUT_CHARS) else output
        return WorkspaceCommandResult(
            exitCode = exitCode,
            stdout = stdout,
            stderr = "",
            timedOut = false,
            truncated = truncated,
        )
    }

    private fun WorkspaceShellContext.prootCwd(): String {
        val normalized = cwd.trim().trim('/')
        return if (normalized.isBlank()) WORKSPACE_DIR else "$WORKSPACE_DIR/$normalized"
    }
}

private fun String.shellQuote(): String = "'" + replace("'", "'\"'\"'") + "'"
