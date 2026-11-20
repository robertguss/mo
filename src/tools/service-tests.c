#define _POSIX_C_SOURCE 200809L

#include "tools/service-tests.h"

#include "base/codes.h"
#include "tools/promises.h"
#include "tools/recovery.h"
#include "tools/run.h"

#include <string.h>
#include <time.h>

struct judge {
    uint32_t item;
    uint32_t function;
    uint32_t tool;
    struct mo_promise_vacuity vacuity;
    uint64_t seen_mask;
    bool active;
};

struct run {
    const struct mo_cli *cli;
    const struct mo_program *program;
    struct mo_image *image;
    const struct mo_sources *sources;
    const struct mo_observation *items;
    uint32_t count;
    uint32_t service;
    const struct mo_bytecode *file;
    const struct mo_types *types;
    struct mo_recovery *recovery;
    struct mo_process *process;
    struct judge judges[MO_LIMIT_SERVICE_PROMISES];
    uint32_t judge_count;
    uint32_t recovery_item;
    const char *problem;
    struct timespec start;
    bool last;
    struct mo_cell values[MO_CLI_RECOVERY_STEPS];
    uint32_t types_kept[MO_CLI_RECOVERY_STEPS];
    struct mo_process *stopped;
    struct mo_defect defect;
    bool decided[MO_LIMIT_SERVICE_PROMISES + 1];
    struct mo_buffer records[MO_LIMIT_SERVICE_PROMISES + 1];
    struct mo_test_totals *totals;
    struct mo_diags *diags;
};

static bool copy_in(struct run *w, uint32_t type, struct mo_cell value, struct mo_cell *out) {
    size_t length = 0;
    const char *failure = NULL;
    return mo_value_copy_saved(mo_recovery_heap(w->recovery), mo_process_heap(w->process), type,
                               value, out, &length, &failure);
}

static bool reply_value(struct run *w, const struct mo_recovery_observation *o, uint32_t type,
                        struct mo_cell *out) {
    uint32_t shown;
    struct mo_cell value =
        mo_recovery_reply(w->recovery, o->step->tool, o->step->sent, o->reply, &shown);
    if (shown == type)
        return copy_in(w, type, value, out);
    const struct mo_type *result = &w->types->items[type];
    uint32_t tag = o->step->sent == MO_SENT_REJECTED ? 1 : 0;
    if (tag == 1) {
        struct mo_cell payload = mo_cell_get_payload(value);
        value = mo_case_shape(w->types, shown, 1) == MO_CASE_BOXED ? mo_value_field(payload, 0)
                                                                   : payload;
        shown = w->types->items[shown].cases[1].fields[0].type;
    }
    MO_ASSERT(result->kind == MO_TYPE_CHOICE && result->cases[tag].fields[0].type == shown);
    struct mo_cell payload;
    if (!copy_in(w, shown, value, &payload))
        return false;
    struct mo_heap *heap = mo_process_heap(w->process);
    if (mo_value_case_owned(heap, type, tag, &payload, 1, out))
        return true;
    mo_value_drop(heap, shown, payload);
    return false;
}

static bool arguments(struct run *w, const struct judge *j, const struct mo_recovery_observation *o,
                      struct mo_cell *out, uint32_t *made) {
    const struct mo_bytecode_function *f = &w->file->functions[j->function];
    struct mo_heap *heap = mo_process_heap(w->process);
    uint32_t state = w->file->services[w->service].state_type;
    if (!mo_value_decode(heap, state, o->before->bytes, o->before->length, &out[*made]))
        return false;
    (*made)++;
    if (!mo_value_decode(heap, state, o->after->bytes, o->after->length, &out[*made]))
        return false;
    (*made)++;
    out[(*made)++] = mo_cell_time(mo_simulation_now(o->simulation));
    if (j->tool == MO_NO_INDEX)
        return true;
    const struct mo_bytecode_service_tool *tool = &w->file->services[w->service].tools[j->tool];
    for (uint32_t a = 0; a < tool->argument_count; a++) {
        if (!copy_in(w, tool->argument_types[a], o->step->arguments[a], &out[*made]))
            return false;
        (*made)++;
    }
    if (!reply_value(w, o, f->parameters[f->parameter_count - 1].type, &out[*made]))
        return false;
    (*made)++;
    return true;
}

static bool work_remaining(void *context) {
    uint64_t *remaining = context;
    if (*remaining == 0)
        return false;
    (*remaining)--;
    return true;
}

static bool returned(struct run *w, struct judge *j, const struct mo_recovery_observation *o,
                     uint32_t *line) {
    const struct mo_bytecode_function *f = &w->file->functions[j->function];
    struct mo_heap *heap = mo_process_heap(w->process);
    struct mo_cell values[MO_LIMIT_BYTECODE_PARAMETERS];
    uint32_t made = 0;
    bool ready = arguments(w, j, o, values, &made);
    if (ready) {
        MO_ASSERT(made == f->parameter_count);
        for (uint32_t a = 0; a < made; a++)
            if (f->parameters[a].ownership == MO_PARAMETER_OWNED)
                values[a] = mo_value_dup(heap, f->parameters[a].type, values[a]);
        uint64_t remaining = (uint64_t)MO_LIMIT_SERVICE_WORK + 1;
        struct mo_cell result;
        struct mo_defect defect;
        enum mo_controlled_outcome outcome = mo_call_controlled(
            w->process, j->function, values, made, work_remaining, &remaining, &result, &defect);
        if (outcome == MO_CONTROL_CANCELLED)
            defect = (struct mo_defect){.reason = "running past a service promise's work limit",
                                        .limit_name = "MO_LIMIT_SERVICE_WORK",
                                        .limit = MO_LIMIT_SERVICE_WORK,
                                        .span.file = MO_NO_INDEX};
        if (outcome != MO_CONTROL_RETURNED) {
            if (w->last) {
                MO_ASSERT(w->stopped == NULL);
                w->stopped = w->process;
                w->defect = defect;
            } else
                mo_process_free(w->process);
            w->process = mo_process_new(w->image, &w->cli->limits);
            if (w->process == NULL)
                w->problem = "a fresh process for service promises could not be made";
            return false;
        }
        int64_t answer = mo_cell_get_int(result);
        if (answer > 0) {
            MO_ASSERT((uint64_t)answer <= UINT32_MAX);
            *line = (uint32_t)answer;
        } else {
            *line = 0;
            j->seen_mask |= -(uint64_t)answer;
        }
    } else
        w->problem = "a service promise's values could not be copied into its process";
    for (uint32_t a = 0; a < made; a++)
        mo_value_drop(heap, f->parameters[a].type, values[a]);
    MO_ASSERT(!ready || mo_heap_objects(heap) == 0);
    return true;
}

static struct mo_recovery_verdict judge_step(const struct mo_recovery_observation *o,
                                             void *context) {
    struct run *w = context;
    struct mo_recovery_verdict verdict = {MO_NO_INDEX, 0};
    if (w->problem != NULL || o->before == NULL || o->after == NULL)
        return verdict;
    for (uint32_t i = 0; i < w->judge_count; i++) {
        struct judge *j = &w->judges[i];
        if (!j->active ||
            (j->tool != MO_NO_INDEX &&
             (o->step->kind != MO_RECOVERY_SEND || o->step->tool != j->tool || o->reply == NULL)))
            continue;
        uint32_t line = 0;
        bool body = returned(w, j, o, &line);
        if (w->problem != NULL)
            return verdict;
        if (!body || line != 0)
            return (struct mo_recovery_verdict){i, line};
    }
    return verdict;
}

static void keep_reply(const struct mo_recovery_observation *o, void *context) {
    struct run *w = context;
    if (o->reply == NULL)
        return;
    uint32_t type;
    struct mo_cell value =
        mo_recovery_reply(w->recovery, o->step->tool, o->step->sent, o->reply, &type);
    w->values[o->index] = mo_value_dup(mo_recovery_heap(w->recovery), type, value);
    w->types_kept[o->index] = type;
}

static uint64_t elapsed(const struct run *w) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < w->start.tv_sec)
        return 0;
    int64_t us = ((int64_t)now.tv_sec - (int64_t)w->start.tv_sec) * 1000000 +
                 (now.tv_nsec - w->start.tv_nsec) / 1000;
    return us > 0 ? (uint64_t)us : 0;
}

static bool decide(struct run *w, uint32_t item, const char *problem, const char *detail,
                   struct mo_test_result *result) {
    MO_ASSERT(!w->decided[item]);
    w->decided[item] = true;
    const struct mo_observation *observation = &w->items[item];
    if (problem != NULL) {
        mo_test_report_error(w->diags, problem, mo_observation_span(observation),
                             detail != NULL ? detail : observation->name);
        return true;
    }
    result->observation = observation;
    result->file = w->file;
    result->microseconds = elapsed(w);
    w->totals->passed += result->outcome == MO_RETURNED;
    w->totals->failed += result->outcome == MO_FAILED;
    w->totals->failed_tests += result->outcome == MO_FAILED && observation->promise == NULL;
    w->totals->stopped += result->outcome == MO_STOPPED;
    w->totals->stopped_tests += result->outcome == MO_STOPPED && observation->promise == NULL;
    if (mo_render_test_result(w->sources, result, w->cli->json, &w->records[item]))
        return true;
    mo_test_report_error(w->diags, "test result could not be rendered or written completely",
                         mo_observation_span(observation), observation->name);
    return false;
}

static void set_judges(struct run *w, uint32_t only) {
    for (uint32_t i = 0; i < w->judge_count; i++)
        w->judges[i].active = !w->decided[w->judges[i].item] && (only == MO_NO_INDEX || i == only);
}

static bool failed(struct run *w, uint32_t item, struct mo_recovery_result *found,
                   struct mo_recovery_counts counts) {
    bool check = found->outcome == MO_RECOVERY_CHECK;
    mo_recovery_compare_restarts(w->recovery, !check);
    if (check)
        set_judges(w, found->verdict.check);
    struct mo_recovery_hooks hooks = {NULL, check ? judge_step : NULL, w};
    if (!w->cli->first_failure) {
        uint32_t runs = mo_recovery_shrink(w->recovery, &found->sequence, found->outcome,
                                           check ? &hooks : NULL, found->verdict);
        (void)runs;
    }
    for (uint32_t i = 0; i < MO_CLI_RECOVERY_STEPS; i++)
        w->types_kept[i] = MO_NO_INDEX;
    w->last = true;
    hooks.observe = keep_reply;
    struct mo_recovery_result rerun;
    enum mo_recovery_outcome outcome =
        mo_recovery_replay(w->recovery, &found->sequence, &hooks, &rerun);
    w->last = false;
    bool same = outcome == found->outcome && rerun.verdict.check == found->verdict.check &&
                rerun.verdict.line == found->verdict.line;
    counts.sequences++;
    const struct mo_checked_service_promise *promise = w->items[item].promise;
    struct mo_recovery_report report = {.recovery = w->recovery,
                                        .service = w->service,
                                        .counts = counts,
                                        .sequence = &found->sequence,
                                        .result = &rerun,
                                        .replies = w->values,
                                        .reply_types = w->types_kept};
    struct mo_test_result result = {.outcome = MO_FAILED,
                                    .line.file = UINT32_MAX,
                                    .recovery = &report,
                                    .process = w->stopped,
                                    .defect = &w->defect};
    if (check && rerun.verdict.line == 0) {
        MO_ASSERT(w->stopped != NULL || !same);
        result.outcome = MO_STOPPED;
    } else if (check)
        result.line = promise->spec.conditions[rerun.verdict.line - 1].text.span;
    const char *problem = w->problem != NULL ? "service promises could not run"
                          : same             ? NULL
                                             : "the shrunk recovery sequence did not fail the "
                                               "same way when it ran again";
    bool written = decide(w, item, problem, w->problem, &result);
    for (uint32_t i = 0; i < MO_CLI_RECOVERY_STEPS; i++)
        if (w->types_kept[i] != MO_NO_INDEX)
            mo_value_drop(mo_recovery_heap(w->recovery), w->types_kept[i], w->values[i]);
    if (w->stopped != NULL)
        mo_process_free(w->stopped);
    w->stopped = NULL;
    mo_recovery_result_free(w->recovery, &rerun);
    return written;
}

static void add(struct mo_recovery_counts *to, struct mo_recovery_counts from) {
    to->sequences += from.sequences;
    to->restarts += from.restarts;
    to->commits += from.commits;
    to->replayed += from.replayed;
    to->snapshots += from.snapshots;
}

static void harvest_tool_examples(struct run *w, struct judge *j) {
    const struct mo_checked_service_promise *promise = w->items[j->item].promise;
    if (promise != NULL) {
        const char *mod = w->program->set->names[promise->spec.module];
        for (uint32_t k = 0; k < promise->anchor_count &&
                             (j->seen_mask & j->vacuity.required_mask) != j->vacuity.required_mask;
             k++) {
            const struct mo_checked_spec *anchor = &promise->anchors[k];
            if (!anchor->allows)
                continue;
            uint32_t fn = mo_service_function(w->file, mod, promise->spec.number, anchor);
            if (fn == MO_NO_INDEX)
                continue;
            struct mo_process *proc = mo_process_new(w->image, &w->cli->limits);
            if (proc == NULL)
                return;
            uint64_t budget = (uint64_t)MO_LIMIT_SERVICE_WORK + 1;
            struct mo_cell ans;
            struct mo_defect defect;
            if (mo_call_controlled(proc, fn, NULL, 0, work_remaining, &budget, &ans, &defect) ==
                MO_CONTROL_RETURNED) {
                int64_t cond = mo_cell_get_int(ans);
                mo_value_drop(mo_process_heap(proc), w->file->functions[fn].result_type, ans);
                if (cond <= 0)
                    j->seen_mask |= -(uint64_t)cond;
            }
            mo_process_free(proc);
        }
    }
    if (j->tool == MO_NO_INDEX ||
        (j->seen_mask & j->vacuity.required_mask) == j->vacuity.required_mask)
        return;
    const struct mo_bytecode_service_tool *btool = &w->file->services[w->service].tools[j->tool];
    uint32_t handler_fn = btool->function;
    enum mo_service_reply reply_kind = w->items[j->item].service->tools[j->tool].reply;
    const struct mo_bytecode_function *pfn = &w->file->functions[j->function];
    const struct mo_bytecode_string *hname = &w->file->strings[w->file->functions[handler_fn].name];
    struct mo_arena obs_arena;
    mo_arena_init(&obs_arena, sizeof(struct mo_call_observer) + 64);
    struct mo_call_observer *observer =
        mo_arena_allocate(&obs_arena, sizeof(*observer), _Alignof(struct mo_call_observer));
    if (observer == NULL) {
        mo_arena_free(&obs_arena);
        return;
    }
    for (unsigned pass = 0;
         pass < 2 && (j->seen_mask & j->vacuity.required_mask) != j->vacuity.required_mask;
         pass++) {
        for (uint32_t ex = 0; ex < w->file->function_count &&
                              (j->seen_mask & j->vacuity.required_mask) != j->vacuity.required_mask;
             ex++) {
            enum mo_function_kind fk = w->file->functions[ex].kind;
            if (fk != MO_FUNCTION_EXAMPLE && fk != MO_FUNCTION_TEST)
                continue;
            const struct mo_bytecode_string *ex_name =
                &w->file->strings[w->file->functions[ex].name];
            bool direct = fk == MO_FUNCTION_EXAMPLE && ex_name->length > hname->length + 9 &&
                          memcmp(ex_name->bytes, hname->bytes, hname->length) == 0 &&
                          memcmp(ex_name->bytes + hname->length, ".example.", 9) == 0;
            if (direct != (pass == 0))
                continue;
            struct mo_process *proc = mo_process_new(w->image, &w->cli->limits);
            if (proc == NULL)
                break;
            *observer = (struct mo_call_observer){.target = handler_fn};
            mo_process_observe_calls(proc, observer);
            uint64_t budget = (uint64_t)MO_LIMIT_SERVICE_WORK + 1;
            struct mo_cell ex_val;
            struct mo_defect defect;
            enum mo_controlled_outcome ex_out =
                mo_call_controlled(proc, ex, NULL, 0, work_remaining, &budget, &ex_val, &defect);
            mo_process_observe_calls(proc, NULL);
            if (ex_out == MO_CONTROL_RETURNED) {
                struct mo_heap *heap = mo_process_heap(proc);
                mo_value_drop(heap, w->file->functions[ex].result_type, ex_val);
                uint32_t reply_ty = pfn->parameters[pfn->parameter_count - 1].type;
                uint32_t hres_ty = w->file->functions[handler_fn].result_type;
                for (uint32_t c = 0;
                     c < observer->count &&
                     (j->seen_mask & j->vacuity.required_mask) != j->vacuity.required_mask;
                     c++) {
                    struct mo_cell before = observer->calls[c].arguments[0];
                    struct mo_cell after = before;
                    struct mo_cell now = observer->calls[c].arguments[1];
                    struct mo_cell reply_cell = mo_cell_unit();
                    struct mo_cell constructed_reply = mo_cell_unit();
                    bool reply_owned = false;
                    if (reply_kind == MO_SERVICE_READ_ONLY)
                        reply_cell = observer->calls[c].result;
                    else if (reply_kind == MO_SERVICE_UPDATE) {
                        after = mo_value_field(observer->calls[c].result, 0);
                        reply_cell = mo_value_field(observer->calls[c].result, 1);
                    } else {
                        uint32_t tag = mo_cell_get_tag(observer->calls[c].result);
                        struct mo_cell raw = mo_cell_get_payload(observer->calls[c].result);
                        struct mo_cell inner =
                            mo_case_shape(w->types, hres_ty, tag) == MO_CASE_BOXED
                                ? mo_value_field(raw, 0)
                                : raw;
                        uint32_t field_ty = w->types->items[reply_ty].cases[tag].fields[0].type;
                        struct mo_cell payload_val = tag == 0 ? mo_value_field(inner, 1) : inner;
                        if (tag == 0)
                            after = mo_value_field(inner, 0);
                        struct mo_cell dup_payload = mo_value_dup(heap, field_ty, payload_val);
                        if (!mo_value_case_owned(heap, reply_ty, tag, &dup_payload, 1,
                                                 &constructed_reply)) {
                            mo_value_drop(heap, field_ty, dup_payload);
                            break;
                        }
                        reply_cell = constructed_reply;
                        reply_owned = true;
                    }
                    struct mo_cell call_args[MO_LIMIT_BYTECODE_PARAMETERS];
                    call_args[0] = before;
                    call_args[1] = after;
                    call_args[2] = now;
                    for (uint32_t a = 0; a < btool->argument_count; a++)
                        call_args[3 + a] = observer->calls[c].arguments[2 + a];
                    call_args[3 + btool->argument_count] = reply_cell;
                    MO_ASSERT(4 + btool->argument_count == pfn->parameter_count);
                    for (uint32_t a = 0; a < pfn->parameter_count; a++)
                        if (pfn->parameters[a].ownership == MO_PARAMETER_OWNED)
                            call_args[a] =
                                mo_value_dup(heap, pfn->parameters[a].type, call_args[a]);
                    budget = (uint64_t)MO_LIMIT_SERVICE_WORK + 1;
                    struct mo_cell pres;
                    enum mo_controlled_outcome pout =
                        mo_call_controlled(proc, j->function, call_args, pfn->parameter_count,
                                           work_remaining, &budget, &pres, &defect);
                    if (reply_owned)
                        mo_value_drop(heap, reply_ty, constructed_reply);
                    if (pout != MO_CONTROL_RETURNED)
                        break;
                    int64_t ans = mo_cell_get_int(pres);
                    mo_value_drop(heap, pfn->result_type, pres);
                    if (ans <= 0)
                        j->seen_mask |= -(uint64_t)ans;
                }
            }
            mo_call_observer_clear(proc, observer);
            mo_process_free(proc);
        }
    }
    mo_arena_free(&obs_arena);
}

static bool search(struct run *w) {
    struct mo_recovery_counts base = {0};
    uint64_t first = 0;
    bool written = true;
    for (;;) {
        bool comparing = w->recovery_item != MO_NO_INDEX && !w->decided[w->recovery_item];
        mo_recovery_compare_restarts(w->recovery, comparing);
        set_judges(w, MO_NO_INDEX);
        struct mo_recovery_hooks hooks = {NULL, w->judge_count != 0 ? judge_step : NULL, w};
        struct mo_recovery_result found;
        enum mo_recovery_outcome outcome =
            mo_recovery_search(w->recovery, first, w->cli->sequences, &hooks, &found);
        struct mo_recovery_counts counts = base;
        add(&counts, found.counts);
        struct mo_recovery_report report = {
            .recovery = w->recovery, .service = w->service, .counts = counts, .result = &found};
        const char *problem = w->problem != NULL ? "service promises could not run" : NULL;
        const char *detail = w->problem;
        enum mo_outcome every = MO_RETURNED;
        bool more = false;
        switch (outcome) {
        case MO_RECOVERY_PASSED:
            break;
        case MO_RECOVERY_STOPPED:
            every = MO_STOPPED;
            break;
        case MO_RECOVERY_UNAVAILABLE:
            problem = "recovery check could not run";
            detail = found.problem;
            break;
        case MO_RECOVERY_MISMATCH:
        case MO_RECOVERY_REFUSED:
            if (problem == NULL)
                more = written = failed(w, w->recovery_item, &found, counts);
            break;
        case MO_RECOVERY_CHECK:
            if (problem == NULL)
                more = written = failed(w, w->judges[found.verdict.check].item, &found, counts);
            break;
        }
        for (uint32_t i = 0; written && !more && i < w->count; i++)
            if (!w->decided[i]) {
                struct mo_test_result result = {
                    .outcome = every, .line.file = UINT32_MAX, .recovery = &report};
                if (every == MO_RETURNED && problem == NULL && w->items[i].promise != NULL) {
                    struct judge *j = NULL;
                    for (uint32_t k = 0; k < w->judge_count; k++)
                        if (w->judges[k].item == i)
                            j = &w->judges[k];
                    MO_ASSERT(j != NULL);
                    if (j->vacuity.tautology_line != 0) {
                        result.outcome = MO_FAILED;
                        result.vacuity = MO_VACUITY_TAUTOLOGY;
                        result.line = w->items[i]
                                          .promise->spec.conditions[j->vacuity.tautology_line - 1]
                                          .text.span;
                    } else if ((j->seen_mask & j->vacuity.required_mask) !=
                               j->vacuity.required_mask) {
                        harvest_tool_examples(w, j);
                        if ((j->seen_mask & j->vacuity.required_mask) != j->vacuity.required_mask) {
                            uint32_t missing =
                                mo_vacuity_missing_line(j->vacuity.required_mask, j->seen_mask);
                            result.outcome = MO_FAILED;
                            result.vacuity = MO_VACUITY_UNREACHED_IMPLIES;
                            result.line =
                                w->items[i].promise->spec.conditions[missing - 1].text.span;
                        }
                    }
                }
                written = decide(w, i, problem, detail, &result);
            }
        add(&base, found.resume);
        first = found.ordinal;
        mo_recovery_result_free(w->recovery, &found);
        if (!more || w->cli->first_failure)
            return written;
    }
}

static bool bind_judges(struct run *w) {
    const struct mo_checked_service *checked = w->items[0].service;
    const struct mo_bytecode_service *service = &w->file->services[w->service];
    const char *module = w->program->set->names[checked->module];
    w->recovery_item = MO_NO_INDEX;
    for (uint32_t i = 0; i < w->count; i++) {
        const struct mo_checked_service_promise *promise = w->items[i].promise;
        MO_ASSERT(w->items[i].service == checked);
        if (promise == NULL) {
            w->recovery_item = i;
            continue;
        }
        uint32_t function = mo_service_function(w->file, module, promise->spec.number, NULL);
        if (function == MO_NO_INDEX)
            return false;
        uint32_t tool = MO_NO_INDEX;
        if (promise->tool != UINT32_MAX) {
            MO_ASSERT(promise->tool < service->tool_count);
            tool = promise->tool;
            const struct mo_syntax *name = checked->tools[tool].name;
            const struct mo_bytecode_string *compiled =
                &w->file->strings[service->tools[tool].name];
            MO_ASSERT(compiled->length == name->span.end - name->span.start &&
                      memcmp(compiled->bytes, name->text, compiled->length) == 0);
        }
        w->judges[w->judge_count++] =
            (struct judge){.item = i,
                           .function = function,
                           .tool = tool,
                           .vacuity = mo_promise_vacuity_plan(w->program, &promise->spec),
                           .active = true};
    }
    return true;
}

bool mo_test_service(const struct mo_cli *cli, const struct mo_program *program,
                     struct mo_image *image, const struct mo_sources *sources,
                     const struct mo_observation *items, uint32_t count, uint32_t service,
                     FILE *output, struct mo_diags *diags, struct mo_test_totals *totals) {
    MO_ASSERT(count != 0 && count <= MO_LIMIT_SERVICE_PROMISES + 1);
    struct run run;
    struct run *w = &run;
    *w = (struct run){.cli = cli,
                      .program = program,
                      .image = image,
                      .sources = sources,
                      .items = items,
                      .count = count,
                      .service = service,
                      .file = mo_image_bytecode(image),
                      .types = mo_image_types(image),
                      .totals = totals,
                      .diags = diags};
    for (uint32_t i = 0; i < count; i++)
        mo_buffer_init(&w->records[i], MO_CLI_DIAGNOSTIC_SIZE);
    const char *problem = NULL;
    if (clock_gettime(CLOCK_MONOTONIC, &w->start) != 0)
        problem = "the check's clock could not be read";
    else if (!bind_judges(w))
        problem = "service promises could not be bound to compiled functions";
    else if (mo_recovery_new(image, service, &cli->limits, &w->recovery) != MO_INPUT_OK)
        problem = "recovery check could not be prepared";
    else if (w->judge_count != 0 && (w->process = mo_process_new(image, &cli->limits)) == NULL)
        problem = "a process for service promises could not be made";
    bool written = true;
    if (problem != NULL)
        for (uint32_t i = 0; i < count; i++)
            written = decide(w, i, problem, NULL, NULL) && written;
    else
        written = search(w);
    for (uint32_t i = 0; written && i < count; i++)
        if (w->records[i].length != 0 &&
            (fwrite(w->records[i].bytes, 1, w->records[i].length, output) != w->records[i].length ||
             fflush(output) != 0)) {
            mo_test_report_error(diags, "test result could not be rendered or written completely",
                                 mo_observation_span(&items[i]), items[i].name);
            written = false;
        }
    for (uint32_t i = 0; i < count; i++)
        mo_buffer_free(&w->records[i]);
    if (w->process != NULL)
        mo_process_free(w->process);
    if (w->recovery != NULL)
        mo_recovery_free(w->recovery);
    return written;
}
