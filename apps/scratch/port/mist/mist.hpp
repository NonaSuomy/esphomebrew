#pragma once
#include <functional>
#include <memory>
#include <string>
// Cooperative PAX transport for the Mist/TurboWarp cloud protocol. No curl or
// worker thread; tick() runs on the interpreter task so callbacks are
// serialized.
class MistConnection {
public:
  using VariableUpdateCallback =
      std::function<void(const std::string &, const std::string &)>;
  using ConnectionStatusCallback =
      std::function<void(bool, const std::string &)>;
  MistConnection(const std::string &, const std::string &, const std::string &);
  MistConnection(const std::string &, const std::string &, const std::string &,
                 const std::string &);
  ~MistConnection();
  bool connect(bool secure = true);
  void disconnect();
  bool set(const std::string &, const std::string &);
  std::string get(const std::string &) const;
  void onVariableUpdate(VariableUpdateCallback);
  void onConnectionStatus(ConnectionStatusCallback);
  void tick();
  bool isConnected() const;
  bool isConnecting() const;

private:
  struct Impl;
  std::unique_ptr<Impl> p;
};
void pax_cloud_tick();

bool pax_cloud_begin_startup();
bool pax_cloud_startup_waiting();
void pax_cloud_draw_startup();
