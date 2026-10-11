// Tests the CI orb's webhook plugin against the untrusted input it receives, and its keep-alive
// lease. Run with `bun test ci/orb`; ci/check.sh runs it.
import { afterEach, beforeEach, expect, spyOn, test } from 'bun:test'
import { createHmac } from 'node:crypto'
import * as fs from 'node:fs' // the namespace, so that tests can spy on its functions
import { mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { basename, dirname, join } from 'node:path'

type Handler = (event: any, ctx: any) => Promise<void>
type Interval = { callback: () => unknown; milliseconds: number }

const secret = 'test-secret'
const head = 'a'.repeat(40)
const other = 'b'.repeat(40)
const third = 'c'.repeat(40)
const repository = { full_name: 'robertguss/mo' }

let home = ''
let handler: Handler | null = null
let registeredKey = ''

// What the fake Amp saw: keep-alive leases, log lines, dispose callbacks and intervals.
let leases = { requested: 0, granted: 0, released: 0 }
let keepAliveFailure: Error | null = null
let keepAliveDelay: Promise<void> | null = null
let logged: string[] = []
let disposers: Array<() => void | Promise<void>> = []
let intervals = new Map<number, Interval>()
let nextIntervalId = 1

// The plugin checks its lease every 10 seconds. Instead of waiting, the tests record its
// intervals and run them by hand with tick(); nothing real is ever scheduled.
const realSetInterval = globalThis.setInterval
const realClearInterval = globalThis.clearInterval

const held = () => leases.granted - leases.released
const jobName = (sha: string) => `robertguss__mo__${sha}.json`
const directory = (name: string) => join(home, 'mo-ci', name)

async function loadPlugin(enabled: boolean, prepare: () => void = () => {}): Promise<void> {
	home = mkdtempSync(join(tmpdir(), 'mo-ci-test-'))
	process.env.MO_CI_HOME = join(home, 'mo-ci')
	if (enabled) {
		mkdirSync(join(home, 'mo-ci'), { recursive: true })
		writeFileSync(join(home, 'mo-ci', 'enabled'), '')
		writeFileSync(join(home, 'mo-ci', 'webhook-secret'), `${secret}\n`)
	}
	prepare()
	handler = null
	const plugin = await import(`../../.amp/plugins/mo-ci.ts?home=${encodeURIComponent(home)}`)
	await plugin.default({
		logger: { log: (...args: unknown[]) => void logged.push(args.map(String).join(' ')) },
		system: {
			executor: {
				keepAlive: async () => {
					leases.requested++
					if (keepAliveDelay !== null) await keepAliveDelay
					if (keepAliveFailure !== null) throw keepAliveFailure
					leases.granted++
					return { unsubscribe: () => void leases.released++ }
				},
			},
		},
		onDispose: (callback: () => void | Promise<void>) => {
			disposers.push(callback)
			return { unsubscribe: () => {} }
		},
		createWebhook: async (options: { key: string; handler: Handler }) => {
			registeredKey = options.key
			handler = options.handler
			return { url: 'https://example.invalid/webhook/capability' }
		},
	})
}

function delivery(kind: string, payload: unknown, sign = true, id = 'delivery-1') {
	const body = new TextEncoder().encode(typeof payload === 'string' ? payload : JSON.stringify(payload))
	const headers: Record<string, string> = { 'x-github-event': kind }
	if (sign) headers['x-hub-signature-256'] = `sha256=${createHmac('sha256', secret).update(body).digest('hex')}`
	return { id, body, headers, receivedAt: '2026-10-11T12:00:00Z' }
}

const context = () => ({ logger: { log: () => {} }, signal: new AbortController().signal })

async function deliver(event: ReturnType<typeof delivery>): Promise<string[]> {
	expect(handler).not.toBeNull()
	await handler!(event, context())
	return readdirSync(directory('queue')).sort()
}

// Lets work that is waiting on promises finish, such as the lease requested when the plugin loads.
const settle = () => new Promise<void>((resolve) => setTimeout(resolve, 0))

// Runs the plugin's interval callbacks once, as the timer would, and waits for them.
async function tick(): Promise<void> {
	for (const { callback } of [...intervals.values()]) await callback()
}

async function dispose(): Promise<void> {
	for (const callback of disposers.splice(0)) await callback()
}

// What the runner does to a job file.
const moveJob = (name: string, from: string, to: string) =>
	fs.renameSync(join(directory(from), name), join(directory(to), name))

const push = (overrides: Record<string, unknown> = {}) => ({
	ref: 'refs/heads/main',
	after: head,
	deleted: false,
	repository,
	...overrides,
})

const pullRequest = (action: string, headRepo = 'robertguss/mo') => ({
	action,
	number: 7,
	repository,
	pull_request: { head: { sha: head, ref: 'feature', repo: { full_name: headRepo } } },
})

beforeEach(() => {
	registeredKey = ''
	leases = { requested: 0, granted: 0, released: 0 }
	keepAliveFailure = null
	keepAliveDelay = null
	logged = []
	disposers = []
	intervals = new Map()
	globalThis.setInterval = ((callback: () => unknown, milliseconds: number) => {
		const id = nextIntervalId++
		intervals.set(id, { callback, milliseconds })
		return id
	}) as unknown as typeof setInterval
	globalThis.clearInterval = ((id: number) => void intervals.delete(id)) as unknown as typeof clearInterval
})

afterEach(async () => {
	await dispose()
	const leaked = intervals.size
	globalThis.setInterval = realSetInterval
	globalThis.clearInterval = realClearInterval
	if (home !== '') rmSync(home, { recursive: true, force: true })
	home = ''
	expect(leaked).toBe(0) // disposing the plugin must clear every interval it started
})

test('does nothing outside the CI orb', async () => {
	await loadPlugin(false)
	expect(handler).toBeNull()
	expect(disposers.length).toBe(0)
	expect(intervals.size).toBe(0)
	expect(leases.requested).toBe(0)
})

test('registers the github webhook and keeps its URL private', async () => {
	await loadPlugin(true)
	expect(registeredKey).toBe('github-2')
	const urlFile = join(home, 'mo-ci', 'webhook-url')
	expect(readFileSync(urlFile, 'utf8')).toBe('https://example.invalid/webhook/capability')
	expect(statSync(urlFile).mode & 0o777).toBe(0o600)
})

test('queues one job for a signed push to a branch', async () => {
	await loadPlugin(true)
	const queued = await deliver(delivery('push', push()))
	expect(queued).toEqual([jobName(head)])
	const job = JSON.parse(readFileSync(join(directory('queue'), queued[0]!), 'utf8'))
	expect(job).toMatchObject({ repo: 'robertguss/mo', sha: head, ref: 'refs/heads/main', event: 'push' })
})

test('a redelivery adds no second job', async () => {
	await loadPlugin(true)
	await deliver(delivery('push', push()))
	const queued = await deliver(delivery('push', push(), true, 'delivery-2'))
	expect(queued).toEqual([jobName(head)])
	const job = JSON.parse(readFileSync(join(directory('queue'), queued[0]!), 'utf8'))
	expect(job.deliveryId).toBe('delivery-1') // the first delivery's job is kept
})

test('ignores pull request events: the head commit arrives as a push', async () => {
	await loadPlugin(true)
	for (const action of ['opened', 'synchronize', 'reopened', 'edited', 'closed']) {
		expect(await deliver(delivery('pull_request', pullRequest(action)))).toEqual([])
	}
	expect(await deliver(delivery('pull_request', pullRequest('opened', 'someone/fork')))).toEqual([])
	expect(leases.requested).toBe(0)
	// The branch push that opened the pull request builds it, once.
	expect(await deliver(delivery('push', push({ ref: 'refs/heads/feature' })))).toEqual([jobName(head)])
	expect(await deliver(delivery('pull_request', pullRequest('synchronize'), true, 'delivery-2'))).toEqual([
		jobName(head),
	])
})

for (const stage of ['running', 'done']) {
	test(`does not queue a commit that is already in ${stage}/`, async () => {
		await loadPlugin(true)
		writeFileSync(join(directory(stage), jobName(head)), '{}')
		expect(await deliver(delivery('push', push()))).toEqual([])
		expect(readdirSync(directory(stage))).toEqual([jobName(head)])
		// Another commit is still built.
		expect(await deliver(delivery('push', push({ after: other })))).toEqual([jobName(other)])
	})
}

test('the queue holds only complete .json jobs', async () => {
	await loadPlugin(true)
	const commits = ['1', '2', '3', '4'].map((digit) => digit.repeat(40))
	const events = [
		...commits.flatMap((sha, index) => [
			delivery('push', push({ after: sha }), true, `push-${index}`),
			delivery('push', push({ after: sha }), true, `again-${index}`), // redelivered
			delivery('pull_request', pullRequest('synchronize'), true, `pull-${index}`),
		]),
		delivery('push', push({ deleted: true }), true, 'deleted'),
		delivery('push', 'not json', true, 'garbage'),
		delivery('push', push(), false, 'unsigned'),
	]
	for (const event of events) {
		const names = await deliver(event)
		// After every delivery, nothing but finished jobs is there to be claimed.
		for (const name of names) {
			expect(name).toMatch(/^robertguss__mo__[0-9a-f]{40}\.json$/)
			const job = JSON.parse(readFileSync(join(directory('queue'), name), 'utf8'))
			expect(job).toMatchObject({ repo: 'robertguss/mo', ref: 'refs/heads/main', event: 'push' })
			expect(name).toBe(`robertguss__mo__${job.sha}.json`)
			expect(typeof job.deliveryId).toBe('string')
			expect(typeof job.receivedAt).toBe('string')
		}
	}
	expect(readdirSync(directory('queue')).sort()).toEqual(commits.map(jobName))
})

test('publishes each job by renaming a finished temporary file within the queue directory', async () => {
	await loadPlugin(true)
	const rename = fs.renameSync
	const renames: Array<{ from: string; to: string; listing: string[]; content: string }> = []
	const spy = spyOn(fs, 'renameSync').mockImplementation(((from: string, to: string) => {
		// What a runner looking at the queue at this very moment would see.
		renames.push({ from, to, listing: readdirSync(dirname(to)), content: readFileSync(from, 'utf8') })
		rename(from, to)
	}) as typeof fs.renameSync)
	try {
		await deliver(delivery('push', push()))
	} finally {
		spy.mockRestore()
	}
	expect(renames.length).toBe(1)
	const { from, to, listing, content } = renames[0]!
	expect(dirname(from)).toBe(directory('queue')) // the same directory, so the rename is atomic
	expect(dirname(to)).toBe(directory('queue'))
	expect(basename(to)).toBe(jobName(head))
	expect(from.endsWith('.json')).toBe(false) // the runner claims *.json files only
	expect(listing).toEqual([basename(from)]) // the job is not visible under its final name yet...
	expect(JSON.parse(content)).toMatchObject({ repo: 'robertguss/mo', sha: head }) // ...until it is complete
	expect(readdirSync(directory('queue'))).toEqual([jobName(head)]) // and no temporary file is left
})

test('leaves nothing in the queue when a job cannot be published, so a retry can queue it', async () => {
	await loadPlugin(true)
	const spy = spyOn(fs, 'renameSync').mockImplementation((() => {
		throw new Error('disk full')
	}) as typeof fs.renameSync)
	try {
		await expect(handler!(delivery('push', push()), context())).rejects.toThrow('disk full')
	} finally {
		spy.mockRestore()
	}
	expect(readdirSync(directory('queue'))).toEqual([])
	expect(await deliver(delivery('push', push(), true, 'delivery-2'))).toEqual([jobName(head)])
})

test('keeps the orb awake from the moment a job is queued until all jobs are done', async () => {
	await loadPlugin(true)
	expect(leases.requested).toBe(0) // nothing to wait for yet
	const [{ milliseconds }] = [...intervals.values()] as [Interval]
	expect(milliseconds).toBeGreaterThanOrEqual(5_000)
	expect(milliseconds).toBeLessThanOrEqual(15_000)

	await deliver(delivery('push', push()))
	expect(held()).toBe(1) // acquired as soon as the job is queued
	await deliver(delivery('push', push({ after: other }), true, 'delivery-2'))
	expect(leases.requested).toBe(1) // one lease covers both jobs

	// The runner claims and finishes the first job; the second is still waiting.
	moveJob(jobName(head), 'queue', 'running')
	await tick()
	moveJob(jobName(head), 'running', 'done')
	await tick()
	expect(held()).toBe(1)

	// It claims the second: running counts as busy too.
	moveJob(jobName(other), 'queue', 'running')
	await tick()
	expect(held()).toBe(1)

	// Both directories are empty: the lease is released.
	moveJob(jobName(other), 'running', 'done')
	await tick()
	expect(held()).toBe(0)
	expect(leases.released).toBe(1)
	await tick()
	expect(leases.released).toBe(1) // and only once

	// The next job takes a new lease.
	await deliver(delivery('push', push({ after: third }), true, 'delivery-3'))
	expect(leases.requested).toBe(2)
	expect(held()).toBe(1)
})

test('checks its lease against the job directories on a timer', async () => {
	// A job left in running/ before the plugin loaded, as when the orb wakes up mid-run.
	await loadPlugin(true, () => {
		mkdirSync(join(home, 'mo-ci', 'running'), { recursive: true })
		writeFileSync(join(home, 'mo-ci', 'running', jobName(head)), '{}')
	})
	await settle()
	expect(held()).toBe(1)

	// A directory that is missing counts as empty; other files are not jobs.
	rmSync(directory('running'), { recursive: true })
	writeFileSync(join(directory('queue'), 'notes.txt'), '')
	await tick()
	expect(held()).toBe(0)
	expect(logged.filter((line) => line.includes('keep-alive'))).toEqual([])

	// A job that appears without a webhook delivery is noticed by the next check.
	writeFileSync(join(directory('queue'), jobName(other)), '{}')
	await tick()
	expect(held()).toBe(1)
})

test('logs and carries on when keepAlive rejects', async () => {
	keepAliveFailure = new Error('not running inside an Amp-managed orb')
	await loadPlugin(true)
	expect(await deliver(delivery('push', push()))).toEqual([jobName(head)]) // the webhook still works
	expect(held()).toBe(0)
	const complaint = logged.find((line) => line.includes('keep-alive'))
	expect(complaint).toContain('not running inside an Amp-managed orb')
	expect(await deliver(delivery('push', push({ after: other }), true, 'delivery-2'))).toEqual([
		jobName(head),
		jobName(other),
	])

	// The next check tries again.
	keepAliveFailure = null
	await tick()
	expect(held()).toBe(1)
})

test('releases the lease and clears its timer when disposed', async () => {
	await loadPlugin(true)
	await deliver(delivery('push', push()))
	expect(held()).toBe(1)
	expect(intervals.size).toBe(1)
	expect(disposers.length).toBe(1)

	await dispose()
	expect(held()).toBe(0)
	expect(leases.released).toBe(1)
	expect(intervals.size).toBe(0)

	// A delivery that straggles in after disposal must not take a lease nobody would release.
	await deliver(delivery('push', push({ after: other }), true, 'delivery-2'))
	expect(leases.requested).toBe(1)
})

test('releases a lease that is granted only after the plugin is disposed', async () => {
	let grant = () => {}
	keepAliveDelay = new Promise<void>((resolve) => (grant = resolve))
	await loadPlugin(true)
	const delivered = deliver(delivery('push', push()))
	await settle()
	expect(leases.requested).toBe(1) // keepAlive() has been asked, but has not answered
	await dispose()
	grant()
	await delivered
	expect(leases.granted).toBe(1)
	expect(held()).toBe(0)
})

test('ignores forged, unsigned and wrongly signed deliveries', async () => {
	await loadPlugin(true)
	expect(await deliver(delivery('push', push(), false))).toEqual([])
	const forged = delivery('push', push())
	forged.headers['x-hub-signature-256'] = `sha256=${'0'.repeat(64)}`
	expect(await deliver(forged)).toEqual([])
	const short = delivery('push', push())
	short.headers['x-hub-signature-256'] = 'sha256=abc'
	expect(await deliver(short)).toEqual([])
	expect(leases.requested).toBe(0)
})

test('ignores what it should not build', async () => {
	await loadPlugin(true)
	const ignored = [
		delivery('push', push({ deleted: true })),
		delivery('push', push({ ref: 'refs/tags/v0.1' })),
		delivery('push', push({ after: '0'.repeat(39) })),
		delivery('push', push({ after: `${'a'.repeat(39)};` })),
		delivery('push', push({ repository: { full_name: 'someone/else' } })),
		delivery('pull_request', pullRequest('opened')),
		delivery('ping', { zen: 'Keep it logically awesome.', repository }),
		delivery('push', 'not json'),
	]
	for (const event of ignored) expect(await deliver(event)).toEqual([])
	expect(leases.requested).toBe(0)
})
