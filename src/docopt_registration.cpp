/** \file docopt_registration.cpp

Functions for handling the set of docopt descriptions

*/

#include "config.h"
#include "docopt_registration.h"
#include "common.h"
#include "parse_constants.h"
#include "parser.h"
#include "docopt_fish.h"
#include "expand.h"
#include <map>
#include <vector>
#include <list>
#include <set>
#include <memory>
#include <algorithm>

typedef docopt_fish::base_annotated_option_t<wcstring> docopt_option_t;
typedef docopt_fish::argument_parser_t<wcstring> docopt_parser_t;
typedef docopt_fish::error_t docopt_error_t;
typedef std::vector<docopt_error_t> docopt_error_list_t;
typedef docopt_parser_t::argument_map_t docopt_argument_map_t;

/* Weird function. Given a parser status and an existing argument status, convert the parser status to the argument status and return the "more valid" of the two. This supports our design for multiple parsers, where if any parser declares an argument valid, that argument is marked valid. */
enum docopt_argument_status_t more_valid_status(docopt_fish::argument_status_t parser_status, docopt_argument_status_t existing_status)
{
    enum docopt_argument_status_t new_status = static_cast<enum docopt_argument_status_t>(parser_status);
    switch (existing_status)
    {
        case status_invalid: return new_status;
        case status_valid: return status_valid;
        case status_valid_prefix: return new_status == status_valid ? new_status : existing_status;
        default: assert(0 && "Unknown argument status"); return status_invalid;
    }
}

// Given a variable name like <hostname>, return a description like Hostname
static wcstring description_from_variable_name(const wcstring &var)
{
    // Remove < and >. Replace _ with space.
    // Hack: Do nothing unless we find an alphabetic character. This is for our legacy
    // support where we create variable names that are just numbers
    bool found_alpha = false;
    wcstring result = var;
    for (size_t i = 0; i < result.size(); i++)
    {
        wchar_t c = result.at(i);
        if (c == L'<' || c == L'>')
        {
            result.erase(i, 1);
            i -= 1;
        }
        else if (c == L'_')
        {
            result.at(i) = L' ';
        }
        found_alpha = found_alpha || isalpha(c);
    }
    
    // Uppercase the first character
    if (! found_alpha)
    {
        // It's just a number, not going to use it
        result.clear();
    }
    else if (! result.empty())
    {
        result.at(0) = towupper(result.at(0));
    }
    return result;
}

static void append_parse_error(parse_error_list_t *out_errors, size_t where, const wcstring &text)
{
    if (out_errors != NULL)
    {
        out_errors->resize(out_errors->size() + 1);
        parse_error_t *parse_err = &out_errors->back();
        parse_err->text = text;
        parse_err->code = parse_error_docopt;
        parse_err->source_start = where;
        parse_err->source_length = 0;
    }
}

/* Represents a single docopt registration. This is immutable. */
class docopt_registration_t
{
    friend class doc_register_t;
    friend class docopt_registration_set_t;
    /* No copying */
    void operator=(docopt_registration_t&);
    docopt_registration_t(const docopt_registration_t&);
    
    docopt_registration_t() : handle(0), last_arg_only(false) {}
    
    docopt_registration_handle_t handle;
    wcstring usage;
    wcstring description;
    wcstring condition;
    shared_ptr<docopt_fish::argument_parser_t<wcstring> > parser;
    
    // Legacy completion support: completions have historically only looked
    // at the last argument to decide what to do. If set, we follow suit, allowing
    // us to emulate the old behavior.
    bool last_arg_only;
};

// Class that holds a mapping from command name to list of docopt descriptions
class doc_register_t {
    typedef std::map<wcstring, docopt_registration_set_t> registration_map_t;
    registration_map_t cmd_to_registration;
    docopt_registration_handle_t last_handle;
    mutex_lock_t lock;
    
    // Looks for errors in the parser variable commands
    static bool validate_parser(const docopt_parser_t &parser, parse_error_list_t *out_errors)
    {
        bool success = true;
        const wcstring_list_t vars = parser.get_variables();
        parser_t error_detector(PARSER_TYPE_ERRORS_ONLY, false);
        for (size_t i=0; i < vars.size(); i++)
        {
            const wcstring &var = vars.at(i);
            const wcstring command_string = parser.metadata_for_name(var).command;
            if (! command_string.empty())
            {
                wcstring local_err;
                if (error_detector.detect_errors_in_argument_list(command_string, &local_err, L""))
                {
                    wcstring err_text = format_string(L"Command '%ls' contained a syntax error:\n%ls", command_string.c_str(), local_err.c_str());
                    // TODO: would be nice to have the actual position of the error
                    append_parse_error(out_errors, -1, err_text);
                    success = false;
                    break;
                }
            }
        }
        return success;
    }
    
    public:
    bool register_usage(const wcstring &cmd_or_empty, const wcstring &condition, const wcstring &usage, const wcstring &description, parse_error_list_t *out_errors, docopt_registration_handle_t *out_handle)
    {
        // Try to parse it
        shared_ptr<docopt_parser_t> parser(new docopt_parser_t());
        docopt_error_list_t errors;
        bool success = parser->set_doc(usage, &errors);
        
        // Verify it
        success = success && validate_parser(*parser, out_errors);

        // Translate errors from docopt to parse_error over
        if (out_errors != NULL)
        {
            for (size_t i=0; i < errors.size(); i++)
            {
                const docopt_error_t &doc_err = errors.at(i);
                append_parse_error(out_errors, doc_err.location, str2wcstring(doc_err.text));
            }
        }

        // If the command is empty, we determine the command by inferring it from the doc, if there is one
        wcstring effective_cmd = cmd_or_empty;
        if (effective_cmd.empty())
        {
            const wcstring_list_t cmd_names = parser->get_command_names();
            if (cmd_names.empty())
            {
                append_parse_error(out_errors, 0, L"No command name found in docopt description");
            }
            else if (cmd_names.size() > 1)
            {
                const wchar_t *first = cmd_names.at(0).c_str();
                const wchar_t *second = cmd_names.at(1).c_str();
                const wcstring text = format_string(L"Multiple command names found in docopt description, such as '%ls' and '%ls'", first, second);
                append_parse_error(out_errors, 0, text);
            }
            else
            {
                assert(cmd_names.size() == 1);
                effective_cmd = cmd_names.front();
            }
        }
        success = success && ! effective_cmd.empty();
        
        if (success)
        {
            // Ok, we're going to insert it!
            scoped_lock locker(lock);
            docopt_registration_set_t &regs = cmd_to_registration[effective_cmd];
            
            // Remove any with a matching usage
            typedef std::vector<shared_ptr<const docopt_registration_t> >::iterator reg_iter_t;
            reg_iter_t iter = regs.registrations.begin();
            while (iter != regs.registrations.end())
            {
                if ((*iter)->usage == usage)
                {
                    iter = regs.registrations.erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
            
            // Determine the handle
            docopt_registration_handle_t handle = ++this->last_handle;
            
            // Create our registration
            // We will transfer ownership to a shared_ptr
            docopt_registration_t *reg = new docopt_registration_t();
            
            reg->handle = handle;
            reg->usage = usage;
            reg->description = description;
            reg->condition = condition;
            reg->parser = parser; // note shared_ptr
            
            // insert in the front
            // this transfers ownership!
            regs.registrations.insert(regs.registrations.begin(), shared_ptr<const docopt_registration_t>(reg));
            
            if (out_handle)
            {
                *out_handle = handle;
            }
        }
        return success;
    }
        
    void unregister(docopt_registration_handle_t handle)
    {
        if (handle == 0)
        {
            // 0 is never valid
            return;
        }
        scoped_lock locker(lock);
        
        // Need something better than this linear search
        for (registration_map_t::iterator iter = this->cmd_to_registration.begin();
             iter != this->cmd_to_registration.end();
             ++iter)
        {
            docopt_registration_set_t *regs = &iter->second;
            size_t idx = regs->registrations.size();
            while (idx--)
            {
                if (regs->registrations.at(idx)->handle == handle)
                {
                    regs->registrations.erase(regs->registrations.begin() + idx);
                }
            }
        }
    }
    
    docopt_registration_set_t get_registrations(const wcstring &cmd)
    {
        scoped_lock locker(lock);
        registration_map_t::const_iterator where = this->cmd_to_registration.find(cmd);
        if (where == this->cmd_to_registration.end())
        {
            return docopt_registration_set_t();
        }
        else
        {
            return where->second;
        }
    }
    
    doc_register_t() : last_handle(0) {}
};
static doc_register_t default_register;

bool docopt_register_usage(const wcstring &cmd, const wcstring &name, const wcstring &usage, const wcstring &description, parse_error_list_t *out_errors, docopt_registration_handle_t *out_handle)
{
    return default_register.register_usage(cmd, name, usage, description, out_errors, out_handle);
}

void docopt_unregister(docopt_registration_handle_t handle)
{
    default_register.unregister(handle);
}

docopt_registration_set_t docopt_get_registrations(const wcstring &cmd)
{
    return default_register.get_registrations(cmd);
}

docopt_registration_set_t::~docopt_registration_set_t()
{
}

std::vector<docopt_argument_status_t> docopt_registration_set_t::validate_arguments(const wcstring_list_t &argv, docopt_parse_flags_t flags) const
{
    std::vector<docopt_argument_status_t> result;
    result.reserve(argv.size());
    
    // For each parser, have it validate the arguments. Mark an argument as the most valid that any parser declares it to be.
    for (size_t i=0; i < registrations.size(); i++)
    {
        const docopt_parser_t *p = registrations.at(i)->parser.get();
        const std::vector<docopt_fish::argument_status_t> parser_statuses = p->validate_arguments(argv, flags);
        
        // Fill result with status_invalid until it's the right size
        if (result.size() <= parser_statuses.size())
        {
            result.insert(result.end(), parser_statuses.size() - result.size(), status_invalid);
        }
        assert(result.size() >= parser_statuses.size());
        
        for (size_t i=0; i < parser_statuses.size(); i++)
        {
            result.at(i) = more_valid_status(parser_statuses.at(i), result.at(i));
        }
    }
    return result;
}

static bool suggestionNameLessThan(const docopt_suggestion_t &a, const docopt_suggestion_t &b) {
    return a.token < b.token;
}

static bool suggestionNameEquals(const docopt_suggestion_t &a, const docopt_suggestion_t &b) {
    return a.token == b.token;
}


std::vector<docopt_suggestion_t> docopt_registration_set_t::suggest_next_argument(const wcstring_list_t &argv, docopt_parse_flags_t flags) const
{
    /* Construct a list of the last argument only, for last_arg_only support */
    wcstring_list_t last_arg;
    if (! argv.empty())
    {
        last_arg.push_back(argv.back());
    }

    /* Include results from all registered parsers. We have to capture metadata here. */
    std::vector<docopt_suggestion_t> result;
    for (size_t i=0; i < registrations.size(); i++)
    {
        const docopt_registration_t *reg = registrations.at(i).get();
        const docopt_parser_t *parser = reg->parser.get();
        const wcstring_list_t &effective_argv = reg->last_arg_only ? last_arg : argv;
        const wcstring_list_t tmp = parser->suggest_next_argument(effective_argv, flags);
        result.reserve(result.size() + tmp.size());
        for (size_t i=0; i < tmp.size(); i++)
        {
            const wcstring &name = tmp.at(i);
            docopt_parser_t::metadata_t md = parser->metadata_for_name(name);
            
            result.push_back(docopt_suggestion_t());
            docopt_suggestion_t *sugg = &result.back();
            sugg->token = name;
            sugg->command.swap(md.command);
            sugg->condition.swap(md.condition);
            sugg->description.swap(md.description);
            sugg->tag = md.tag;
            
            // Maybe derive a description
            if (sugg->description.empty() && string_prefixes_string(L"<", name))
            {
                sugg->description = description_from_variable_name(name);
            }
        }
    }
    
    /* Sort and remove duplicates */
    std::sort(result.begin(), result.end(), suggestionNameLessThan);
    result.erase(std::unique(result.begin(), result.end(), suggestionNameEquals), result.end());
    
    return result;
}

bool docopt_registration_set_t::parse_arguments(const wcstring_list_t &argv, docopt_arguments_t *out_arguments, parse_error_list_t *out_errors, std::vector<size_t> *out_unused_arguments) const
{
    // Common case?
    if (this->registrations.empty())
    {
        return false;
    }
    
    const bool wants_unused = (out_unused_arguments != NULL);
    
    // An argument is unused if it is unused in all cases
    // This is the union of all unused arguments.
    // Initially all are unused - prepopulate it with every index.
    const size_t argv_size = argv.size();
    std::vector<size_t> total_unused_args;
    docopt_arguments_t total_args;
    
    total_unused_args.reserve(argv_size);
    for (size_t i=0; i < argv_size; i++)
    {
        total_unused_args.push_back(i);
    }
    
    // Now run over the docopt parser list
    // TODO: errors!
    for (size_t i=0; i < registrations.size(); i++)
    {
        docopt_error_list_t errors;
        std::vector<size_t> local_unused_args;
        docopt_argument_map_t args = registrations.at(i)->parser->parse_arguments(argv, docopt_fish::flags_default, &errors, (wants_unused ? &local_unused_args : NULL));
        
        // Insert values from the argument map. Don't overwrite, so that earlier docopts take precedence
        docopt_argument_map_t::const_iterator iter;
        for (iter = args.begin(); iter != args.end(); ++iter)
        {
            // We could use insert() to avoid the two lookups, but the code is very ugly
            const wcstring &key = iter->first;
            const docopt_parser_t::argument_t &arg = iter->second;
            if (total_args.has(key))
            {
                // The argument was already present, and we don't overwrite.
                continue;
            }
            
            // Determine what value we want to store
            if (string_prefixes_string(L"<", key))
            {
                // It's a variable. Store its values.
                total_args.vals[key] = iter->second.values;
            }
            else
            {
                // It's an or a command option. Store its count.
                total_args.vals[key] = wcstring_list_t(1, to_string(arg.count));
            }
        }
        
        if (wants_unused)
        {
            // Intersect unused arguments
            std::vector<size_t> intersected_unused_args;
            std::set_intersection(local_unused_args.begin(), local_unused_args.end(),
                                  total_unused_args.begin(), total_unused_args.end(),
                                  std::back_inserter(intersected_unused_args));
            total_unused_args.swap(intersected_unused_args);
        }
    }
    
    if (out_arguments != NULL)
    {
        out_arguments->swap(total_args);
    }
    if (out_unused_arguments != NULL)
    {
        out_unused_arguments->swap(total_unused_args);
    }
    
    return true;
}

void docopt_registration_set_t::add_legacy_parser(const shared_ptr<docopt_fish::argument_parser_t<wcstring> > &r)
{
    docopt_registration_t *reg = new docopt_registration_t();
    reg->last_arg_only = true;
    reg->parser = r;
    // Note this line transfers ownership
    this->registrations.push_back(shared_ptr<const docopt_registration_t>(reg));
}

/* Returns a reference to the value in the map for the given key, or NULL */
const wcstring_list_t *docopt_arguments_t::get_list_internal(const wcstring &key) const
{
    std::map<wcstring, wcstring_list_t>::const_iterator where = this->vals.find(key);
    return where != this->vals.end() ? &where->second : NULL;
}

bool docopt_arguments_t::has(const wchar_t *key) const
{
    return this->vals.find(key) != this->vals.end();
}

bool docopt_arguments_t::has(const wcstring &key) const
{
    return this->vals.find(key) != this->vals.end();
}

const wcstring_list_t &docopt_arguments_t::get_list(const wchar_t *key) const
{
    static const wcstring_list_t empty;
    const wcstring_list_t *result = this->get_list_internal(key);
    return result ? *result : empty;
}

const wcstring &docopt_arguments_t::get(const wchar_t *key) const
{
    static const wcstring empty;
    const wcstring_list_t &result = this->get_list(key);
    return result.empty() ? empty : result.at(0);
}

const wchar_t *docopt_arguments_t::get_or_null(const wchar_t *key) const
{
    const wcstring_list_t &result = this->get_list(key);
    return result.empty() ? NULL : result.at(0).c_str();
}

wcstring docopt_arguments_t::dump() const
{
    wcstring result;
    for (std::map<wcstring, wcstring_list_t>::const_iterator iter = this->vals.begin(); iter != this->vals.end(); ++iter)
    {
        append_format(result, L"arg: %ls -> %lu\n", iter->first.c_str(), iter->second.size());
        for (size_t i=0; i < iter->second.size(); i++)
        {
            append_format(result, L"\t%ls\n", iter->second.at(i).c_str());
        }
    }
    return result;
}

void docopt_arguments_t::swap(docopt_arguments_t &rhs)
{
    this->vals.swap(rhs.vals);
}

wcstring docopt_derive_variable_name(const wcstring &key)
{
    assert(! key.empty());
    wcstring result = key;
    if (result.at(0) == L'-')
    {
        // It's an option. Strip leading dashes, prepend 'opt_'
        size_t first_non_dash = result.find_first_not_of(L'-');
        if (first_non_dash == wcstring::npos)
        {
            first_non_dash = result.size(); //paranoia?
        }
        result.replace(0, first_non_dash, L"opt_");
    }
    else if (result.at(0) == L'<')
    {
        // Variable. Strip leading < and >
        assert(result.at(result.size() - 1) == L'>');
        result.erase(result.end() - 1);
        result.erase(result.begin());
    }
    else
    {
        // A command. Prepend 'cmd_'.
        result.insert(0, L"cmd_");
    }
    
    // We always replace dashes with underscores
    std::replace(result.begin(), result.end(), L'-', L'_');
    return result;
}