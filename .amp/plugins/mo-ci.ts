import type { PluginAPI, Subscription } from '@ampcode/plugin'
import { createHmac, timingSafeEqual } from 'node:crypto'
import { existsSync, mkdirSync, readdirSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs'
import { homedir } from 'node:os'
import { join } from 'node:path'

export const description =
	"Mo's CI (decision D208). In the dedicated CI orb only, receives GitHub push webhooks, queues one check run per commit for ci/orb/runner.sh, and keeps the orb awake while jobs are waiting or running. Does nothing in any other orb."

// Everything lives outside the repository, in the CI orb's home directory
// (MO_CI_HOME overrides it, for tests).
const home = process.env.MO_CI_HOME ?? join(homedir(), 'mo-ci')
const enabledFile = join(home, 'enabled') // created only in the CI orb
const secretFile = join(home, 'webhook-secret') // shared with GitHub; owner-only
const urlFile = join(home, 'webhook-url') // a credential; owner-only, never printed
// A job's file moves from queue to running to done as ci/orb/runner.sh works on it.
const queueDir = join(home, 'queue')
const runningDir = join(home, 'running')
const doneDir = join(home, 'done')

// Repositories this CI builds. mo-v2 joins once its checks move here.
const repositories = new Set(['robertguss/mo'])

// How often the keep-alive lease is checked against the job directories.
const leaseCheckMilliseconds = 10_000

type Job = {
	repo: string
	sha: string
	ref: string
	event: 'push'
	deliveryId: string
	receivedAt: string
}

export default async function (amp: PluginAPI) {
	if (!existsSync(enabledFile)) return
	for (const dir of [queueDir, runningDir, doneDir]) mkdirSync(dir, { recursive: true, mode: 0o700 })

	// The orb is kept awake while any job waits or runs, so it is never paused under the runner.
	let lease: Subscription | null = null
	let acquiring = false
	let disposed = false

	function release(): void {
		const held = lease
		lease = null
		held?.unsubscribe()
	}

	async function acquire(): Promise<void> {
		if (lease !== null || acquiring || disposed) return
		acquiring = true
		try {
			const granted = await amp.system.executor.keepAlive()
			// Disposed while the lease was being granted: nothing would release it later.
			if (disposed) granted.unsubscribe()
			else lease = granted
		} finally {
			acquiring = false
		}
	}

	// Brings the lease in line with the job directories. Never rejects: a failure, such as
	// keepAlive() rejecting, is logged, and the next check tries again.
	async function reconcileLease(): Promise<void> {
		try {
			if (hasJobs(queueDir) || hasJobs(runningDir)) await acquire()
			else release()
		} catch (error) {
			amp.logger.log('mo-ci: could not update the keep-alive lease:', String(error))
		}
	}

	const timer = setInterval(() => reconcileLease(), leaseCheckMilliseconds)
	amp.onDispose(() => {
		disposed = true
		clearInterval(timer)
		release()
	})

	const { url } = await amp.createWebhook({
		key: 'github',
		headers: ['x-github-event', 'x-hub-signature-256'],
		handler: async (event, ctx) => {
			if (!signatureMatches(event.body, event.headers['x-hub-signature-256'])) {
				ctx.logger.log('mo-ci: ignored a delivery without a valid signature', event.id)
				return
			}
			const job = toJob(event.headers['x-github-event'], parse(event.body), event.id, event.receivedAt)
			if (job === null) return
			if (enqueue(job)) ctx.logger.log('mo-ci: queued', job.repo, job.sha)
			else ctx.logger.log('mo-ci: skipped a commit that is already queued or built', job.repo, job.sha)
			// The job is safely queued; the lease can fail without losing it.
			await reconcileLease()
		},
	})
	writeFileSync(urlFile, url, { mode: 0o600 })
	// Jobs may already be waiting, for example when the orb has just woken up.
	void reconcileLease()
}

function hasJobs(dir: string): boolean {
	try {
		return readdirSync(dir).some((name) => name.endsWith('.json'))
	} catch (error) {
		if ((error as NodeJS.ErrnoException).code === 'ENOENT') return false
		throw error
	}
}

// Queues the job unless its commit has already been queued, run or finished. Returns whether it
// was queued. Each commit is built once: a redelivered event, or a branch pushed at a commit
// that was already built, adds nothing.
function enqueue(job: Job): boolean {
	const name = `${job.repo.replace('/', '__')}__${job.sha}.json`
	// The runner moves a job from queue to running to done. Looking in that order cannot miss a
	// job that is moving on while we look.
	if ([queueDir, runningDir, doneDir].some((dir) => existsSync(join(dir, name)))) return false
	// Write the whole job next to its final name, then rename: the runner, which claims every
	// *.json file it sees, can never pick up a half-written one.
	const file = join(queueDir, name)
	const partial = `${file}.${process.pid}.tmp`
	try {
		writeFileSync(partial, JSON.stringify(job), { mode: 0o600 })
		renameSync(partial, file)
	} catch (error) {
		rmSync(partial, { force: true })
		throw error
	}
	return true
}

function signatureMatches(body: Uint8Array, header: string | undefined): boolean {
	if (header === undefined || !existsSync(secretFile)) return false
	const secret = readFileSync(secretFile, 'utf8').trim()
	if (secret.length === 0) return false
	const expected = Buffer.from(`sha256=${createHmac('sha256', secret).update(body).digest('hex')}`)
	const actual = Buffer.from(header)
	return actual.length === expected.length && timingSafeEqual(actual, expected)
}

function parse(body: Uint8Array): any {
	try {
		return JSON.parse(new TextDecoder().decode(body))
	} catch {
		return null
	}
}

function isSha(value: unknown): value is string {
	return typeof value === 'string' && /^[0-9a-f]{40}$/.test(value)
}

// The payload is untrusted input: read only the fields a job needs, and check each.
//
// Only pushes build. Every pull request in this repository comes from one of its own
// branches, so its head commit always arrives as a push; forks are not accepted.
function toJob(kind: string | undefined, payload: any, deliveryId: string, receivedAt: string): Job | null {
	if (kind !== 'push') return null
	const repo = payload?.repository?.full_name
	if (typeof repo !== 'string' || !repositories.has(repo)) return null
	const ref = payload.ref
	if (payload.deleted === true || !isSha(payload.after)) return null
	if (typeof ref !== 'string' || !ref.startsWith('refs/heads/')) return null
	return { repo, sha: payload.after, ref, event: 'push', deliveryId, receivedAt }
}
