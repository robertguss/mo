// Tests the CI orb's webhook plugin against the untrusted input it receives.
// Run with `bun test ci/orb`; ci/check.sh runs it.
import { afterEach, beforeEach, expect, test } from 'bun:test'
import { createHmac } from 'node:crypto'
import { mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

type Handler = (event: any, ctx: any) => Promise<void>

const secret = 'test-secret'
let home = ''
let handler: Handler | null = null
let registeredKey = ''

const head = 'a'.repeat(40)
const repository = { full_name: 'robertguss/mo' }

async function loadPlugin(enabled: boolean): Promise<void> {
	home = mkdtempSync(join(tmpdir(), 'mo-ci-test-'))
	process.env.MO_CI_HOME = join(home, 'mo-ci')
	if (enabled) {
		mkdirSync(join(home, 'mo-ci'), { recursive: true })
		writeFileSync(join(home, 'mo-ci', 'enabled'), '')
		writeFileSync(join(home, 'mo-ci', 'webhook-secret'), `${secret}\n`)
	}
	handler = null
	const plugin = await import(`../../.amp/plugins/mo-ci.ts?home=${encodeURIComponent(home)}`)
	await plugin.default({
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

async function deliver(event: ReturnType<typeof delivery>): Promise<string[]> {
	expect(handler).not.toBeNull()
	await handler!(event, { logger: { log: () => {} }, signal: new AbortController().signal })
	return readdirSync(join(home, 'mo-ci', 'queue'))
}

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
})

afterEach(() => {
	rmSync(home, { recursive: true, force: true })
})

test('does nothing outside the CI orb', async () => {
	await loadPlugin(false)
	expect(handler).toBeNull()
})

test('registers the github webhook and keeps its URL private', async () => {
	await loadPlugin(true)
	expect(registeredKey).toBe('github')
	const urlFile = join(home, 'mo-ci', 'webhook-url')
	expect(readFileSync(urlFile, 'utf8')).toBe('https://example.invalid/webhook/capability')
	expect(statSync(urlFile).mode & 0o777).toBe(0o600)
})

test('queues one job for a signed push to a branch', async () => {
	await loadPlugin(true)
	const queued = await deliver(delivery('push', push()))
	expect(queued).toEqual([`robertguss__mo__${head}.json`])
	const job = JSON.parse(readFileSync(join(home, 'mo-ci', 'queue', queued[0]!), 'utf8'))
	expect(job).toMatchObject({ repo: 'robertguss/mo', sha: head, ref: 'refs/heads/main', event: 'push' })
})

test('a redelivery or a pull request for the same commit adds no second job', async () => {
	await loadPlugin(true)
	await deliver(delivery('push', push()))
	await deliver(delivery('push', push()))
	const queued = await deliver(delivery('pull_request', pullRequest('synchronize'), true, 'delivery-2'))
	expect(queued.length).toBe(1)
})

test('queues opened, synchronized and reopened pull requests from the repository', async () => {
	for (const action of ['opened', 'synchronize', 'reopened']) {
		await loadPlugin(true)
		expect((await deliver(delivery('pull_request', pullRequest(action)))).length).toBe(1)
		rmSync(home, { recursive: true, force: true })
	}
	await loadPlugin(true)
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
})

test('ignores what it should not build', async () => {
	await loadPlugin(true)
	const ignored = [
		delivery('push', push({ deleted: true })),
		delivery('push', push({ ref: 'refs/tags/v0.1' })),
		delivery('push', push({ after: '0'.repeat(39) })),
		delivery('push', push({ after: `${'a'.repeat(39)};` })),
		delivery('push', push({ repository: { full_name: 'someone/else' } })),
		delivery('pull_request', pullRequest('closed')),
		delivery('pull_request', pullRequest('opened', 'someone/fork')),
		delivery('ping', { zen: 'Keep it logically awesome.', repository }),
		delivery('push', 'not json'),
	]
	for (const event of ignored) expect(await deliver(event)).toEqual([])
})
