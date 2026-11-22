import { useForm } from "@inertiajs/react";
import type { FormEvent } from "react";

interface LoginProps {
  allowlist_hint: string;
  errors: Record<string, string>;
  flash: { notice: string | null; alert: string | null };
}

export default function Login({ allowlist_hint, errors, flash }: LoginProps) {
  const form = useForm({ email: "", password: "" });

  function submit(e: FormEvent) {
    e.preventDefault();
    form.post("/login");
  }

  return (
    <main className="mx-auto max-w-sm p-6">
      <h1 className="text-2xl font-semibold">Sign in to Arvo</h1>
      <p className="mt-1 text-sm text-slate-600">{allowlist_hint}</p>
      {flash.alert && <p className="mt-3 text-sm text-red-600">{flash.alert}</p>}
      <form onSubmit={submit} className="mt-6 space-y-4">
        <div>
          <label className="block text-sm font-medium">Email</label>
          <input
            type="email"
            value={form.data.email}
            onChange={(e) => form.setData("email", e.target.value)}
            className="mt-1 w-full rounded border px-3 py-2"
          />
          {errors.email && <p className="mt-1 text-xs text-red-600">{errors.email}</p>}
        </div>
        <div>
          <label className="block text-sm font-medium">Password</label>
          <input
            type="password"
            value={form.data.password}
            onChange={(e) => form.setData("password", e.target.value)}
            className="mt-1 w-full rounded border px-3 py-2"
          />
          {errors.password && <p className="mt-1 text-xs text-red-600">{errors.password}</p>}
        </div>
        <button
          type="submit"
          disabled={form.processing}
          className="w-full rounded bg-slate-900 py-2 text-white"
        >
          Sign in
        </button>
      </form>
    </main>
  );
}
