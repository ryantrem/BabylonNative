#include <AndroidExtensions/Globals.h>
#include <stdexcept>

namespace android::global
{
    namespace
    {
        JavaVM* g_javaVM{};
        jobject g_appContext{};

        thread_local struct Env
        {
            ~Env()
            {
                if (m_attached)
                {
                    g_javaVM->DetachCurrentThread();
                }
            }

            bool m_attached{};
        } g_env{};

        template<typename ... Args>
        class Event
        {
        public:
            using Handler = std::function<void(Args ...)>;
            using Ticket = typename arcana::ticketed_collection<Handler>::ticket;
            Ticket AddHandler(Handler&& handler)
            {
                std::lock_guard<std::mutex> guard{m_mutex};
                return m_handlers.insert(handler, m_mutex);
            }

            void Fire(Args ... args)
            {
                std::lock_guard<std::mutex> guard{m_mutex};
                for (auto& handler : m_handlers)
                {
                    handler(args ...);
                }
            }

        private:
            std::mutex m_mutex{};
            arcana::ticketed_collection<Handler> m_handlers{};
        };

        using AppStateChangedEvent = Event<>;
        AppStateChangedEvent g_pauseEvent{};
        AppStateChangedEvent g_resumeEvent{};

        using RequestPermissionsResultEvent = Event<int32_t, const std::vector<std::string>&, const std::vector<int32_t>&>;
        RequestPermissionsResultEvent g_requestPermissionsResultEvent{};
    }

    void Initialize(JavaVM* javaVM, jobject appContext)
    {
        g_javaVM = javaVM;
        g_appContext = appContext;
    }

    JNIEnv* GetEnvForCurrentThread()
    {
        JNIEnv* env{};

        if (g_javaVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED)
        {
            if (g_javaVM->AttachCurrentThread(&env, nullptr) != 0) {
                throw std::runtime_error("Failed to attach JavaScript thread to Java VM");
            }

            g_env.m_attached = true;
        }

        return env;
    }

    android::content::Context GetAppContext()
    {
        return {g_appContext};
    }

    void Pause()
    {
        g_pauseEvent.Fire();
    }

    AppStateChangedEvent::Ticket AddPauseCallback(AppStateChangedEvent::Handler&& onPause)
    {
        return g_pauseEvent.AddHandler(std::move(onPause));
    }

    void Resume()
    {
        g_resumeEvent.Fire();
    }

    AppStateChangedEvent::Ticket AddResumeCallback(AppStateChangedEvent::Handler&& onResume)
    {
        return g_resumeEvent.AddHandler(std::move(onResume));
    }

    void RequestPermissionsResult(int32_t requestCode, const std::vector<std::string>& permissions, const std::vector<int32_t>& grantResults)
    {
        g_requestPermissionsResultEvent.Fire(requestCode, permissions, grantResults);
    }

    RequestPermissionsResultEvent::Ticket AddRequestPermissionsResultCallback(RequestPermissionsResultEvent::Handler&& onAddRequestPermissionsResult)
    {
        return g_requestPermissionsResultEvent.AddHandler(std::move(onAddRequestPermissionsResult));
    }
}
