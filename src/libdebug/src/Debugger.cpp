#include <libdebug/Debugger.h>

using namespace lldb;

static void logging_callback(const char* msg, void* dbg)
{
    printf("LLDB: %s", msg);
}

Debugger::Debugger()
    : m_launch_flags{ eLaunchFlagNone }
{
    SBDebugger::Initialize();
    m_debugger = SBDebugger::Create();
    m_debugger.SetAsync(false);

    ::setbuf(stdin, nullptr);
    ::setbuf(stdout, nullptr);

    m_debugger.SetErrorFileHandle(stderr, false);
    m_debugger.SetOutputFileHandle(stdout, false);
    m_debugger.SetInputFileHandle(stdin, true);
}

Debugger::~Debugger()
{
    SBDebugger::Destroy(m_debugger);
    SBDebugger::Terminate();
}

void Debugger::enable_debug_log(const char* channel, std::vector<const char*> categories)
{
    categories.push_back(nullptr);
    m_debugger.SetLoggingCallback(logging_callback, nullptr);
    m_debugger.EnableLog(channel, categories.data());
}

bool Debugger::library_load(std::string filename)
{
    SBFileSpec remote_image_spec(filename.c_str(), true);
    if (!remote_image_spec.IsValid()) {
        printf("Cannot create file spec: %s\n", filename.c_str());
        return false;
    }

    // Search in know places for the file.
    if (!remote_image_spec.Exists() && !remote_image_spec.ResolveExecutableLocation()) {
        printf("Cannot create file spec: %s\n", filename.c_str());
        return false;
    }

    SBError error;
    uint32_t image_token = m_process.LoadImage(remote_image_spec, error);
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    if (image_token == LLDB_INVALID_IMAGE_TOKEN) {
        printf("Error: LLDB_INVALID_IMAGE_TOKEN\n");
        return false;
    }

    m_libmap[filename] = image_token;
    return true;
}

bool Debugger::library_unload(std::string filename)
{
    if (m_libmap.find(filename) == m_libmap.end()) {
        printf("Error: library %s does not map to a valid image token.", filename.c_str());
        return false;
    }

    uint32_t image_token = m_libmap[filename];
    SBError error = m_process.UnloadImage(image_token);
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    return true;
}

bool Debugger::process_execute(std::string filename)
{
    return process_execute(filename, {}, {}, {});
}

bool Debugger::process_execute(std::string filename, std::vector<std::string> args)
{
    return process_execute(filename, args, {}, {});
}

bool Debugger::process_execute(std::string filename, std::vector<std::string> args, std::vector<std::string> env)
{
    return process_execute(filename, args, env, {});
}

bool Debugger::process_execute(std::string filename, std::vector<std::string> args, std::vector<std::string> env, std::string cwd)
{
    // Create a debugging target using a file path.
    m_target = m_debugger.CreateTarget(filename.c_str());
    if (!m_target.IsValid()) {
        printf("Cannot create target: %s\n", filename.c_str());
        return false;
    }

    // Save the main module.
    m_module = m_target.FindModule(m_target.GetExecutable());
    if (!m_module.IsValid()) {
        printf("Cannot find module: %s\n", filename.c_str());
        return false;
    }

    // Convert from std::vector<std::string> to std::vector<char const *>.
    auto convert = [](const std::vector<std::string>& in) {
        std::vector<char const*> ret;
        for (const auto& element : in) {
            ret.push_back(element.c_str());
        }

        // Mark the end of the array.
        ret.push_back(nullptr);
        return ret;
    };

    // Convert arguments and environment variables.
    auto converted_argv = convert(args);
    auto converted_envp = convert(env);

    // Initialize the main listener.
    m_listener = m_debugger.GetListener();

    SBError error;
    m_process = m_target.Launch(
        m_listener,
        converted_argv.data(),
        converted_envp.data(),
        nullptr, // stdin_path
        nullptr, // stdout_path
        nullptr, // stderr_path
        cwd.empty() ? nullptr : cwd.c_str(),
        m_launch_flags,
        false,
        error);

    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    // Update current thread.
    m_thread = m_process.GetSelectedThread();

    // Register events.
    m_listener.StartListeningForEventClass(
        m_debugger,
        SBTarget::GetBroadcasterClassName(),
        SBTarget::eBroadcastBitModulesLoaded | SBTarget::eBroadcastBitModulesUnloaded);

    m_listener.StartListeningForEventClass(
        m_debugger,
        SBThread::GetBroadcasterClassName(),
        SBThread::eBroadcastBitThreadSuspended);

    m_listener.StartListeningForEventClass(
        m_debugger,
        SBProcess::GetBroadcasterClassName(),
        SBProcess::eBroadcastBitStateChanged | SBProcess::eBroadcastBitSTDERR | SBProcess::eBroadcastBitSTDOUT);

    auto state = m_process.GetState();
    printf("Launched module with pid=%d state=%s!\n", m_process.GetProcessID(), m_debugger.StateAsCString(state));
}

bool Debugger::update_selected_thread()
{
    if (!m_process.IsValid()) {
        return false;
    }

    SBThread thread;
    auto stop_reason = m_thread.GetStopReason();
    if (!m_thread.IsValid() || stop_reason == eStopReasonInvalid || stop_reason == eStopReasonNone) {
        SBThread plan_thread;
        SBThread other_thread;

        for (auto i = 0; i < m_process.GetNumThreads(); i++) {
            thread = m_process.GetThreadAtIndex(i);
            switch (thread.GetStopReason()) {
            case eStopReasonTrace:
            case eStopReasonBreakpoint:
            case eStopReasonWatchpoint:
            case eStopReasonSignal:
            case eStopReasonException:
                if (!other_thread.IsValid())
                    other_thread = thread;
                break;

            case eStopReasonPlanComplete:
                if (!plan_thread.IsValid())
                    plan_thread = thread;
                break;

            case eStopReasonInvalid:
            case eStopReasonNone:
            default:
                break;
            }
        }

        if (plan_thread.IsValid())
            thread = plan_thread;
        else if (other_thread.IsValid())
            thread = other_thread;
        else if (m_thread.IsValid())
            thread = m_thread;
        else
            thread = m_process.GetThreadAtIndex(0);

        if (!thread.IsValid()) {
            printf("Could not update selected thread\n");
            return false;
        }

        m_process.SetSelectedThread(thread);
    }

    return true;
}

void Debugger::main_loop()
{
    bool m_stop = false;

    SBEvent event;
    while (!m_stop) {
        printf("Waiting for event.\n");

        // Get an event.
        if (!m_listener.WaitForEvent(std::numeric_limits<std::uint32_t>::max(), event)) {
            printf("Timedout while waiting for event.\n");
            break;
        }

        if (!event.IsValid()) {
            printf("Invalid event.\n");
            continue;
        }

        if (!event.GetBroadcaster().IsValid()) {
            printf("Invalid event.\n");
            continue;
        }

        printf("Got event: %s\n", SBEvent::GetCStringFromEvent(event));

        // Dispatch event.
        handle_event(event);
    }
}

bool Debugger::handle_event(SBEvent event)
{
    if (lldb::SBProcess::EventIsProcessEvent(event)) {
        return handle_process_event(event);
    }

    if (lldb::SBThread::EventIsThreadEvent(event)) {
        return handle_thread_event(event);
    }

    if (lldb::SBTarget::EventIsTargetEvent(event)) {
        return handle_target_event(event);
    }

    return false;
}

bool Debugger::handle_process_event(SBEvent event)
{
    bool ret = false;

    switch (event.GetType()) {
    case lldb::SBProcess::eBroadcastBitStateChanged:
        ret = handle_process_state_changed(vEvent);
        break;

    case lldb::SBProcess::eBroadcastBitSTDERR:
        ret = handle_process_stderr();
        break;

    case lldb::SBProcess::eBroadcastBitSTDOUT:
        ret = handle_process_stdout();
        break;

    default:
        break;
    }

    return ret;
}

bool Debugger::handle_thread_event(SBEvent event)
{
    bool ret = false;

    switch (event.GetType()) {
    case lldb::SBThread::eBroadcastBitThreadSuspended:
        handle_thread_suspended(event);
        break;

    default:
        break;
    }

    return ret;
}

bool Debugger::handle_target_event(SBEvent event)
{
    bool ret = false;

    switch (event.GetType()) {
    case lldb::SBTarget::eBroadcastBitModulesLoaded:
        handle_target_modules_loaded(event);
        break;

    case lldb::SBTarget::eBroadcastBitModulesUnloaded:
        handle_target_modules_unloaded(event);
        break;

    default:
        break;
    }

    return ret;
}

bool Debugger::handle_target_modules_loaded(event)
{
    auto n_modules = SBTarget::GetNumModulesFromEvent(event);
    for (auto i = 0; i < n_modules; ++i) {
        auto module = SBTarget::GetModuleAtIndexFromEvent(i, event);
        SBStream out;
        module.GetDescription(out);
        printf("Loaded module: %s\n", out.GetData());
    }

    return true;
}

bool Debugger::handle_target_modules_unloaded(event)
{
    auto n_modules = SBTarget::GetNumModulesFromEvent(event);
    for (auto i = 0; i < n_modules; ++i) {
        auto module = SBTarget::GetModuleAtIndexFromEvent(i, event);
        SBStream out;
        module.GetDescription(out);
        printf("Unloaded module: %s\n", out.GetData());
    }

    return true;
}

bool Debugger::handle_process_state_changed(SBEvent event)
{
    lldb::StateType state = lldb::SBProcess::GetStateFromEvent(event);
    if (state == lldb::eStateInvalid) {
        return false;
    }

    lldb::SBProcess process = lldb::SBProcess::GetProcessFromEvent(event);
    if (!process.IsValid()) {
        return false;
    }

    bool ret = false;
    switch (state) {
    case lldb::eStateStopped:
        ret = handle_process_state_stopped(event);
        break;

    case lldb::eStateCrashed:
    case lldb::eStateSuspended:
        ret = handle_process_state_suspended(event);
        break;

    case lldb::eStateRunning:
        ret = handle_process_state_running(event);
        break;

    case lldb::eStateExited:
        ret = handle_process_state_exited(event);
        break;

    default:
        break;
    }

    return ret;
}

bool Debugger::handle_process_stderr(SBEvent event)
{
    // TODO: Implement stderr handling.
    return true;
}

bool Debugger::handle_process_stdout(SBEvent event)
{
    // TODO: Implement stdout handling.
    return true;
}

bool Debugger::handle_process_state_stopped(SBEvent event)
{
    SBProcess process = lldb::SBProcess::GetProcessFromEvent(event);
    if (!process.IsValid()) {
        return false;
    }

    SBThread thread = process.GetSelectedThread();
    if (!thread.IsValid()) {
        return false;
    }

    if (!update_selected_thread()) {
        return false;
    }

    switch (thread.GetStopReason()) {
    case lldb::eStopReasonInvalid:
    case lldb::eStopReasonNone:
    case lldb::eStopReasonTrace:
    case lldb::eStopReasonBreakpoint:
    case lldb::eStopReasonWatchpoint:
    case lldb::eStopReasonSignal:
    case lldb::eStopReasonException:
    case lldb::eStopReasonExec:
    case lldb::eStopReasonPlanComplete:
    case lldb::eStopReasonThreadExiting:
    case lldb::eStopReasonInstrumentation:
    default:
        break;
    }

    return true;
}

bool Debugger::handle_process_state_suspended(SBEvent event)
{
    if (!update_selected_thread()) {
        return false;
    }

    return true;
}

bool Debugger::handle_process_state_running(SBEvent event)
{
    // TODO: Implement.
}

bool Debugger::handle_process_state_exited(SBEvent event)
{
    // TODO: Implement.
}

bool Debugger::handle_thread_suspended(SBEvent event)
{
    // TODO: Implement.
    return true;
}

bool Debugger::process_attach(std::string process_name, bool wait)
{
    m_target = m_debugger.CreateTarget(nullptr);
    if (!m_target.IsValid()) {
        printf("Cannot create target\n");
        return false;
    }

    SBError error;
    SBListener listener = m_debugger.GetListener();
    m_process = m_target.AttachToProcessWithName(listener, process_name.c_str(), wait, error);
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    printf("Attached to pid %d\n", m_process.GetProcessID());
}

bool Debugger::process_attach(lldb::pid_t pid)
{
    m_target = m_debugger.CreateTarget(nullptr);
    if (!m_target.IsValid()) {
        printf("Cannot create target\n");
        return false;
    }

    SBError error;
    SBListener listener = m_debugger.GetListener();
    m_process = m_target.AttachToProcessWithID(listener, pid, error);
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    printf("Attached to pid %d\n", pid);
}

bool Debugger::process_detach()
{
    SBError error = m_process.Detach();
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    return true;
}

bool Debugger::process_load_core(std::string core_name)
{
    m_target = m_debugger.CreateTarget(nullptr);
    if (!m_target.IsValid()) {
        printf("Cannot create target\n");
        return false;
    }

    m_process = m_target.LoadCore(core_name.c_str());
    if (!m_process.IsValid()) {
        printf("Cannot create process from core file\n");
        return false;
    }

    return true;
}

bool Debugger::process_continue()
{
    SBError error = m_process.Continue();
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    return true;
}

bool Debugger::process_kill()
{
    SBError error = m_process.Kill();
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    return true;
}

bool Debugger::process_stop()
{
    SBError error = m_process.Stop();
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    return true;
}

bool Debugger::step_instruction(bool step_over)
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return false;

    return step_instruction(current_thread, step_over);
}

bool Debugger::step_to(uintptr_t address)
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return false;

    return step_to(current_thread, address);
}

bool Debugger::step_out()
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return false;

    return step_out(current_thread);
}

bool Debugger::step_instruction(lldb::SBThread& thread, bool step_over)
{
    thread.StepInstruction(step_over);
    return true;
}

bool Debugger::step_to(lldb::SBThread& thread, uintptr_t address)
{
    thread.RunToAddress(address);
    return true;
}

bool Debugger::step_out(lldb::SBThread& thread)
{
    thread.StepOut();
    return true;
}

std::optional<lldb::SBValueList> Debugger::registers_get()
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return {};

    return registers_get(current_thread);
}

std::optional<SBValueList> Debugger::registers_get(SBThread& thread)
{
    SBFrame frame = thread.GetFrameAtIndex(0);
    if (!frame.IsValid()) {
        printf("Error getting thread frame.\n");
        return {};
    }

    SBValueList registers = frame.GetRegisters();
    if (!registers.IsValid()) {
        printf("Error getting thread registers.\n");
        return {};
    }

    return registers;
}

bool Debugger::registers_set(std::map<std::string, std::string> values)
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return {};

    return registers_set(current_thread, values);
}

bool Debugger::registers_set(lldb::SBThread& thread, std::map<std::string, std::string> values)
{
    for (const auto& value_pair : values) {
        if (!register_set(thread, value_pair.first, value_pair.second)) {
            printf("Error: failed to set register %s value to %s\n", value_pair.first.c_str(), value_pair.second.c_str());
            return false;
        }
    }

    return true;
}

std::optional<lldb::SBValue> Debugger::register_get(std::string register_name)
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return {};

    return register_get(current_thread, register_name);
}

std::optional<lldb::SBValue> Debugger::register_get(lldb::SBThread& thread, std::string register_name)
{
    SBFrame frame = thread.GetFrameAtIndex(0);
    if (!frame.IsValid()) {
        printf("Error getting thread frame.\n");
        return {};
    }

    SBValueList all_registers = frame.GetRegisters();
    if (!all_registers.IsValid()) {
        printf("Error getting thread registers.\n");
        return {};
    }

    std::optional<lldb::SBValue> ret;
    for (auto i = 0; i < all_registers.GetSize(); i++) {
        SBValue register_set = all_registers.GetValueAtIndex(i);
        if (!register_set.IsValid())
            continue;

        SBValue register_value = register_set.GetChildMemberWithName(register_name.c_str());
        if (register_value.IsValid()) {
            ret = register_value;
            break;
        }
    }

    return ret;
}

bool Debugger::register_set(std::string register_name, std::string register_value)
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return {};

    return register_set(current_thread, register_name, register_value);
}

bool Debugger::register_set(lldb::SBThread& thread, std::string register_name, std::string register_value)
{
    auto reg = register_get(thread, register_name);
    if (!reg) {
        printf("Error: cannot get register %s\n", register_name.c_str());
        return false;
    }

    SBError error;
    reg->SetValueFromCString(register_value.c_str(), error);
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    return true;
}

std::optional<uintptr_t> Debugger::set_fp(uintptr_t address)
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return {};

    return set_fp(current_thread, address);
}

std::optional<uintptr_t> Debugger::set_pc(uintptr_t address)
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return {};

    return set_pc(current_thread, address);
}

std::optional<uintptr_t> Debugger::set_sp(uintptr_t address)
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return {};

    return set_sp(current_thread, address);
}

std::optional<uintptr_t> Debugger::set_pc(lldb::SBThread& thread, uintptr_t address)
{
    SBFrame frame = thread.GetFrameAtIndex(0);
    if (!frame.IsValid()) {
        printf("Error getting thread frame.\n");
        return {};
    }

    auto old_pc = frame.GetPC();
    if (!frame.SetPC(address)) {
        printf("Error setting program counter\n");
        return {};
    }

    return old_pc;
}

std::optional<uintptr_t> Debugger::set_sp(lldb::SBThread& thread, uintptr_t address)
{
    auto old_value = get_sp(thread);
    if (!register_set(thread, ARCH_GENERIC_STACK_POINTER, std::to_string(address))) {
        printf("Error: could not set value of stack pointer.\n");
        return {};
    }

    return old_value;
}

std::optional<uintptr_t> Debugger::set_fp(lldb::SBThread& thread, uintptr_t address)
{
    auto old_value = get_fp(thread);
    if (!register_set(thread, ARCH_GENERIC_FRAME_POINTER, std::to_string(address))) {
        printf("Error: could not set value of frame pointer.\n");
        return {};
    }

    return old_value;
}

std::optional<uintptr_t> Debugger::get_pc()
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return {};

    return get_pc(current_thread);
}

std::optional<uintptr_t> Debugger::get_sp()
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return {};

    return get_sp(current_thread);
}

std::optional<uintptr_t> Debugger::get_fp()
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return {};

    return get_fp(current_thread);
}

std::optional<uintptr_t> Debugger::get_pc(lldb::SBThread& thread)
{
    SBFrame frame = thread.GetFrameAtIndex(0);
    if (!frame.IsValid()) {
        printf("Error getting thread frame.\n");
        return {};
    }

    return frame.GetPC();
}

std::optional<uintptr_t> Debugger::get_sp(lldb::SBThread& thread)
{
    SBFrame frame = thread.GetFrameAtIndex(0);
    if (!frame.IsValid()) {
        printf("Error getting thread frame.\n");
        return {};
    }

    return frame.GetSP();
}

std::optional<uintptr_t> Debugger::get_fp(lldb::SBThread& thread)
{
    SBFrame frame = thread.GetFrameAtIndex(0);
    if (!frame.IsValid()) {
        printf("Error getting thread frame.\n");
        return {};
    }

    return frame.GetFP();
}

std::optional<SBBreakpoint> Debugger::breakpoint_add(uintptr_t address)
{
    SBBreakpoint breakpoint = m_target.BreakpointCreateByAddress(address);
    if (!breakpoint.IsValid()) {
        printf("Error adding breakpoint.\n");
        return {};
    }

    return breakpoint;
}

std::optional<SBBreakpoint> Debugger::breakpoint_add(std::string function_name, std::string module_name)
{
    auto module_name_or_null = module_name.empty() ? nullptr : module_name.c_str();
    SBBreakpoint breakpoint = m_target.BreakpointCreateByName(function_name.c_str(), module_name_or_null);
    if (!breakpoint.IsValid()) {
        printf("Error adding breakpoint.\n");
        return {};
    }

    return breakpoint;
}

bool Debugger::breakpoint_del(SBBreakpoint& breakpoint)
{
    auto break_id = breakpoint.GetID();
    if (!m_target.BreakpointDelete(break_id)) {
        printf("Error: could not remove breakpoint.\n");
        return false;
    }

    return true;
}

bool Debugger::breakpoint_enable_all()
{
    return m_target.EnableAllBreakpoints();
}

bool Debugger::breakpoint_disable_all()
{
    return m_target.DisableAllBreakpoints();
}

bool Debugger::breakpoint_delete_all()
{
    return m_target.DeleteAllBreakpoints();
}

bool Debugger::breakpoint_save(std::string filename)
{
    SBFileSpec dest_file(filename.c_str(), true);
    if (!dest_file.IsValid()) {
        return false;
    }

    SBError error = m_target.BreakpointsWriteToFile(dest_file);
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }
}

bool Debugger::breakpoint_load(std::string filename)
{
    SBFileSpec source_file(filename.c_str(), true);
    if (!source_file.IsValid()) {
        return false;
    }

    SBBreakpointList breakpoint_list(m_target);
    SBError error = m_target.BreakpointsCreateFromFile(source_file, breakpoint_list);
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    return true;
}

std::optional<lldb::SBWatchpoint> Debugger::watchpoint_add(uintptr_t address, size_t size, bool read, bool write)
{
    SBError error;
    SBWatchpoint watchpoint = m_target.WatchAddress(address, size, read, write, error);
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return {};
    }

    return watchpoint;
}

bool Debugger::watchpoint_del(lldb::SBWatchpoint& breakpoint)
{
    auto break_id = breakpoint.GetID();
    if (!m_target.DeleteWatchpoint(break_id)) {
        printf("Error: could not remove data breakpoint.\n");
        return false;
    }

    return true;
}

bool Debugger::watchpoint_delete_all()
{
    return m_target.DeleteAllWatchpoints();
}

bool Debugger::watchpoint_enable_all()
{
    return m_target.EnableAllWatchpoints();
}

bool Debugger::watchpoint_disable_all()
{
    return m_target.DisableAllWatchpoints();
}

template <typename T>
bool Debugger::memory_read(uintptr_t address, T* value)
{
    return memory_read(address, value, sizeof(T));
}

template <typename T>
bool Debugger::memory_write(uintptr_t address, T value)
{
    return memory_write(address, &value, sizeof(T));
}

template <typename T>
bool Debugger::memory_write(uintptr_t address, T value, size_t count)
{
    bool ret = true;
    for (auto i = 0; i < count; i++) {
        ret = memory_write<T>(address, value);
        if (!ret) {
            printf("Error: cannot write value to address\n");
            break;
        }

        address += sizeof(T);
    }

    return ret;
}

bool Debugger::memory_read(uintptr_t address, void* buffer, size_t size)
{
    SBError error;
    m_process.ReadMemory(address, buffer, size, error);
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    return true;
}

bool Debugger::memory_write(uintptr_t address, const void* buffer, size_t size)
{
    SBError error;
    m_process.WriteMemory(address, buffer, size, error);
    if (!error.Success()) {
        printf("Error: %s\n", error.GetCString());
        return false;
    }

    return true;
}

std::optional<lldb::SBInstruction> Debugger::instruction_get()
{
    if (auto list = instructions_get(1)) {
        return list->GetInstructionAtIndex(0);
    }

    return {};
}

std::optional<lldb::SBInstruction> Debugger::instruction_get(uintptr_t address)
{
    if (auto list = instructions_get(address, 1)) {
        return list->GetInstructionAtIndex(0);
    }

    return {};
}

std::optional<lldb::SBInstructionList> Debugger::instructions_get(size_t count)
{
    auto pc = get_pc();
    if (!pc) {
        printf("Could not get value of PC.\n");
        return {};
    }

    uintptr_t address = *pc;
    return instructions_get(address, count);
}

std::optional<lldb::SBInstructionList> Debugger::instructions_get(uintptr_t address, size_t count)
{
    SBAddress sb_address(address, m_target);
    if (!sb_address.IsValid()) {
        printf("Could not get instructions from address.\n");
        return {};
    }

    SBInstructionList instructions = m_target.ReadInstructions(sb_address, count, "intel");
    if (!instructions.IsValid()) {
        printf("Could not get instructions from address.\n");
        return {};
    }

    return instructions;
}

std::optional<lldb::SBMemoryRegionInfoList> Debugger::memory_regions()
{
    SBMemoryRegionInfoList regions = m_process.GetMemoryRegions();
    if (!regions.GetSize()) {
        printf("Error: no memory regions available\n");
        return {};
    }

    return regions;
}

std::optional<lldb::SBMemoryRegionInfo> Debugger::memory_region(uintptr_t address)
{
    SBMemoryRegionInfo region_info;
    SBError error = m_process.GetMemoryRegionInfo(address, region_info);
    if (!error.Success()) {
        printf("Error: could not get memory region information\n");
        return {};
    }

    return region_info;
}

std::optional<lldb::SBModule> Debugger::modules_get(std::string name)
{
    for (auto i = 0; i < m_target.GetNumModules(); i++) {
        auto module = m_target.GetModuleAtIndex(i);
        if (!module.IsValid()) {
            continue;
        }

        std::string module_filename(module.GetFileSpec().GetFilename());
        if (name == module_filename) {
            return module;
        }
    }

    return {};
}

std::optional<std::vector<lldb::SBModule>> Debugger::modules_get()
{
    std::vector<lldb::SBModule> ret;
    for (auto i = 0; i < m_target.GetNumModules(); i++) {
        auto module = m_target.GetModuleAtIndex(i);
        if (!module.IsValid()) {
            continue;
        }

        ret.push_back(module);
    }

    if (ret.empty()) {
        printf("No valid modules are loaded\n");
        return {};
    }

    return ret;
}

template <typename T>
bool Debugger::stack_push(T value)
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return false;

    return stack_push<T>(current_thread, value);
}

template <typename T>
bool Debugger::stack_pop(T* value)
{
    auto current_thread = get_thread();
    if (!current_thread.IsValid())
        return false;

    return stack_pop<T>(current_thread, value);
}

template <typename T>
bool Debugger::stack_push(SBThread& thread, T value)
{
    auto current_sp = get_sp(thread);
    if (!current_sp) {
        printf("Error: Could not get value of stack pointer.\n");
        return false;
    }

    auto new_sp = *current_sp - sizeof(T);
    if (!set_sp(thread, new_sp)) {
        printf("Error: Could update value of stack pointer.\n");
        return false;
    }

    if (!memory_write<T>(new_sp, value)) {
        printf("Error: Could write value to stack.\n");
        return false;
    }

    return true;
}

template <typename T>
bool Debugger::stack_pop(SBThread& thread, T* value)
{
    auto current_sp = get_sp(thread);
    if (!current_sp) {
        printf("Error: Could not get value of stack pointer.\n");
        return false;
    }

    if (!memory_read<T>(*current_sp, value)) {
        printf("Error: Could read value from stack.\n");
        return false;
    }

    *current_sp += sizeof(T);
    if (!set_sp(thread, *current_sp)) {
        printf("Error: Could update value of stack pointer.\n");
        return false;
    }

    return true;
}
