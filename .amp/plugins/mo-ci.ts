import type { PluginAPI } from '@ampcode/plugin'
import { createHmac, timingSafeEqual } from 'node:crypto'
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { homedir } from 'node:os'
import { join } from 'node:path'

export const description =
	"Mo's CI (decision D208). In the dedicated CI orb only, receives GitHub push and pull request webhooks and queues one check run per commit for ci/orb/runner.sh. Does nothing in any other orb."

// Everything lives outside the repository, in the CI orb's home directory
// (MO_CI_HOME overrides it, for tests).
const home = process.env.MO_CI_HOME ?? join(homedir(), 'mo-ci')
const enabledFile = join(home, 'enabled') // created only in the CI orb
const secretFile = join(home, 'webhook-secret') // shared with GitHub; owner-only
const urlFile = join(home, 'webhook-url') // a credential; owner-only, never printed
const queueDir = join(home, 'queue')

// Repositories this CI builds. mo-v2 joins once its checks move here.
const repositories = new Set(['robertguss/mo'])

type Job = {
	repo: string
	sha: string
	ref: string
	event: 'push' | 'pull_request'
	deliveryId: string
	receivedAt: string
}

export default async function (amp: PluginAPI) {
	if (!existsSync(enabledFile)) return
	mkdirSync(queueDir, { recursive: true, mode: 0o700 })
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
			// One job per commit: a push and its pull request event share a file,
			// and a redelivered event finds the file already there.
			const file = join(queueDir, `${job.repo.replace('/', '__')}__${job.sha}.json`)
			if (!existsSync(file)) writeFileSync(file, JSON.stringify(job), { mode: 0o600 })
		},
	})
	writeFileSync(urlFile, url, { mode: 0o600 })
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
function toJob(kind: string | undefined, payload: any, deliveryId: string, receivedAt: string): Job | null {
	const repo = payload?.repository?.full_name
	if (typeof repo !== 'string' || !repositories.has(repo)) return null
	if (kind === 'push') {
		const ref = payload.ref
		if (payload.deleted === true || !isSha(payload.after)) return null
		if (typeof ref !== 'string' || !ref.startsWith('refs/heads/')) return null
		return { repo, sha: payload.after, ref, event: 'push', deliveryId, receivedAt }
	}
	if (kind === 'pull_request') {
		if (!['opened', 'synchronize', 'reopened'].includes(payload.action)) return null
		const head = payload.pull_request?.head
		// Only branches of the repository itself: Mo isn't accepting outside contributions yet.
		if (head?.repo?.full_name !== repo || !isSha(head?.sha) || typeof head?.ref !== 'string') return null
		return { repo, sha: head.sha, ref: `refs/heads/${head.ref}`, event: 'pull_request', deliveryId, receivedAt }
	}
	return null
}
