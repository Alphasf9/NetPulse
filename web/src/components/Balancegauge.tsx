export function BalanceGauge({ balanceBytes }: { balanceBytes: number | null }) {
    const mb = balanceBytes === null ? null : balanceBytes / (1024 * 1024);
    return (
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
            <div className="text-gray-400 text-sm mb-1">Remaining Data</div>
            <div className="text-3xl font-bold text-cyan-400">
                {mb === null ? "—" : `${mb.toFixed(2)} MB`}
            </div>
        </div>
    );
}