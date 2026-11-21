import { useState, useCallback, useEffect } from "react";
import { ActivityLog } from "./components/ActivityLog";
import { RechargePanel } from "./components/RechargePanel";
import { NetworkToggle } from "./components/NetworkToggle";
import { BalanceGauge } from "./components/Balancegauge";
import { LoginPanel } from "./components/LoginPanel";
import { ConnectDevice } from "./components/ConnectDevice";
import { useTelemetry, type TelemetryEvent } from "./useTelemetry";
import { api, clearSession, getSession, type Session } from "./api";

export default function App() {
  const [session, setSession] = useState<Session | null>(() => getSession());

  // api.ts fires this when the server says the token is no longer valid
  useEffect(() => {
    const onLogout = () => setSession(null);
    window.addEventListener("np:logout", onLogout);
    return () => window.removeEventListener("np:logout", onLogout);
  }, []);

  if (!session) return <LoginPanel onLoggedIn={setSession} />;

  return (
    <Dashboard
      key={session.token}
      session={session}
      onLogout={() => {
        clearSession();
        setSession(null);
      }}
    />
  );
}

function Dashboard({ session, onLogout }: { session: Session; onLogout: () => void }) {
  const [events, setEvents] = useState<TelemetryEvent[]>([]);
  const [balance, setBalance] = useState<number | null>(null);
  const [on5g, setOn5g] = useState(false);
  const [boundIp, setBoundIp] = useState<string | null>(null);

  // Load current state so the dashboard is correct before any live event arrives
  useEffect(() => {
    api.me().then((me) => {
      setBalance(me.balanceBytes);
      setOn5g(me.on5g);
      setBoundIp(me.boundIp);
    }).catch(() => { /* 401 is handled in api.ts */ });
  }, []);

  const handleEvent = useCallback((e: TelemetryEvent) => {
    setEvents((prev) => [...prev.slice(-199), e]);
    if (e.type === "SUBSCRIBER_CONNECTED") {
      setBalance(e.balance);
      setOn5g(e.on5g);
    }
    if (e.type === "QUOTA_UPDATE") setBalance(e.newBalance);
    if (e.type === "RAT_CHANGED") setOn5g(e.rat === "5G_NR");
  }, []);

  const { connected } = useTelemetry(session.token, handleEvent);

  return (
    <div className="min-h-screen bg-gray-950 text-gray-100 p-6 space-y-6">
      <header className="flex items-center justify-between gap-3 flex-wrap">
        <h1 className="text-2xl font-bold">NetPulse — Network Console</h1>
        <div className="flex items-center gap-3 text-sm">
          <span className="text-gray-400">{session.name} · {session.msisdn}</span>
          <span className={`px-3 py-1 rounded-full ${connected ? "bg-green-900 text-green-400" : "bg-red-900 text-red-400"}`}>
            {connected ? "● LIVE" : "● DISCONNECTED"}
          </span>
          <button onClick={onLogout} className="px-3 py-1 rounded bg-gray-800 hover:bg-gray-700">Log out</button>
        </div>
      </header>

      <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 gap-6">
        <BalanceGauge balanceBytes={balance} />
        <NetworkToggle on5g={on5g} onChanged={setOn5g} />
        <ConnectDevice boundIp={boundIp} onBound={setBoundIp} />
        <RechargePanel msisdn={session.msisdn} onCredited={setBalance} />
      </div>

      <ActivityLog events={events} />
    </div>
  );
}
