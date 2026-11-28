#include "pw/os/sync.hpp"

namespace pw {

Mutex::Mutex() { pthread_mutex_init(&mu_, nullptr); }
Mutex::~Mutex() { pthread_mutex_destroy(&mu_); }
void Mutex::lock() { pthread_mutex_lock(&mu_); }
void Mutex::unlock() { pthread_mutex_unlock(&mu_); }

CondVar::CondVar() { pthread_cond_init(&cv_, nullptr); }
CondVar::~CondVar() { pthread_cond_destroy(&cv_); }
void CondVar::wait(Mutex& m) { pthread_cond_wait(&cv_, m.native()); }
void CondVar::signal() { pthread_cond_signal(&cv_); }
void CondVar::broadcast() { pthread_cond_broadcast(&cv_); }

}  // namespace pw
