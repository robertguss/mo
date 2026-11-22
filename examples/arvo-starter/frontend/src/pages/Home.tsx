import { router, useForm } from "@inertiajs/react";
import type { FormEvent } from "react";

interface Bill {
  id: number;
  name: string;
  category: string;
  amount_cents: number;
  status: "due" | "in_flight" | "paid";
  version: number;
}

interface AuditEntry {
  id: string;
  actor_name: string;
  action: string;
  entity_table: string;
  entity_id: string;
  created_at: string;
}

interface HomeProps {
  auth: { id: string; email: string; display_name: string };
  bills: Bill[];
  total_due_cents: number;
  audit_feed: AuditEntry[];
  errors: Record<string, string>;
  flash: { notice: string | null; alert: string | null };
}

export default function Home({ auth, bills, total_due_cents, audit_feed, errors, flash }: HomeProps) {
  const form = useForm({ name: "", category: "utilities", amount_cents: "" });

  function addBill(e: FormEvent) {
    e.preventDefault();
    form.post("/bills", { onSuccess: () => form.reset() });
  }

  return (
    <main className="mx-auto max-w-xl p-6 space-y-6">
      <header className="flex items-center justify-between">
        <div>
          <h1 className="text-xl font-semibold">{auth.display_name}</h1>
          <p className="text-sm text-slate-600">
            Due this paycheck: ${(total_due_cents / 100).toFixed(2)}
          </p>
        </div>
        <button
          type="button"
          onClick={() => router.post("/logout")}
          className="rounded border px-3 py-1 text-sm"
        >
          Sign out
        </button>
      </header>

      {flash.notice && <p className="rounded bg-emerald-50 p-3 text-sm text-emerald-800">{flash.notice}</p>}

      <section className="space-y-2">
        <h2 className="font-medium">Paycheck Bills</h2>
        <ul className="divide-y rounded border">
          {bills.map((b) => (
            <li key={b.id} className="flex items-center justify-between p-3 text-sm">
              <span>
                {b.name} <span className="text-slate-500">({b.status})</span>
              </span>
              <span>${(b.amount_cents / 100).toFixed(2)}</span>
            </li>
          ))}
        </ul>
      </section>

      <form onSubmit={addBill} className="space-y-3 rounded border p-4">
        <h2 className="font-medium">Add Bill</h2>
        <input
          placeholder="Bill name"
          value={form.data.name}
          onChange={(e) => form.setData("name", e.target.value)}
          className="w-full rounded border px-3 py-2 text-sm"
        />
        {errors.name && <p className="text-xs text-red-600">{errors.name}</p>}
        <input
          placeholder="Amount in cents (e.g. 7500)"
          value={form.data.amount_cents}
          onChange={(e) => form.setData("amount_cents", e.target.value)}
          className="w-full rounded border px-3 py-2 text-sm"
        />
        {errors.amount_cents && <p className="text-xs text-red-600">{errors.amount_cents}</p>}
        <button type="submit" className="rounded bg-slate-900 px-4 py-2 text-sm text-white">
          Save Bill
        </button>
      </form>

      <section className="space-y-1">
        <h2 className="font-medium text-sm">Recent Audit Trail ({audit_feed.length})</h2>
        {audit_feed.slice(0, 5).map((ev) => (
          <p key={ev.id} className="text-xs text-slate-500">
            {ev.actor_name}: {ev.action} ({ev.entity_table}/{ev.entity_id})
          </p>
        ))}
      </section>
    </main>
  );
}
