#include <catch2/catch.hpp>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>

#include "fiber.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-local-addr"
void* fiber_context_get_test(fiber_t *fiber) {
    int fiber_context_get_test_sp_first_var = 0;
    void* fiber_context_get_test_sp_first_var_ptr = &fiber_context_get_test_sp_first_var;
    fiber_context_get(fiber);

    return fiber_context_get_test_sp_first_var_ptr;
}
#pragma GCC diagnostic pop

void fiber_new_test_empty(fiber_t *fiber_from, fiber_t *fiber_to) {
    return;
}

void fiber_context_swap_test_update_user_data_and_swap_back(fiber_t *fiber_from, fiber_t *fiber_to) {
    *(uint64_t*)fiber_from->start_fp_user_data = 1;
    *(uint64_t*)fiber_to->start_fp_user_data = 2;

    fiber_context_swap(fiber_to, fiber_from);
}

int fiber_stack_protection_test_find_memory_protection(void *start_address, void* end_address) {
    FILE *fp;
    int prot_return = -1;
    char line[1024], start_address_str[17] = { 0 }, end_address_str[17] = { 0 };

    sprintf(start_address_str, "%lx", (uintptr_t)start_address);
    sprintf(end_address_str, "%lx", (uintptr_t)end_address);

    fp = fopen("/proc/self/maps", "r");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open /proc/self/maps\n");
        return 01;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        int cmp1 = strncmp(line, start_address_str, strlen(start_address_str));
        int cmp2 = strncmp(line + strlen(start_address_str) + 1, end_address_str, strlen(end_address_str));

        if (cmp1 != 0 || cmp2 != 0) {
            continue;
        }

        size_t prot_offset = strlen(start_address_str) + 1 + strlen(end_address_str) + 1;
        prot_return = 0;
        if (line[prot_offset + 0] == 'r') {
            prot_return |= PROT_READ;
        }
        if (line[prot_offset + 1] == 'w') {
            prot_return |= PROT_WRITE;
        }
        if (line[prot_offset + 2] == 'x') {
            prot_return |= PROT_EXEC;
        }
    }

    fclose(fp);

    return prot_return;
}

TEST_CASE("fiber.c", "[fiber]") {
    size_t page_size = getpagesize();
    size_t stack_size = page_size * 2;

    SECTION("fiber_stack_protection") {
        SECTION("test enabling the memory protection") {
            int protection_flags = -1;
            fiber_t fiber = { NULL };
            fiber.stack_base = aligned_alloc(page_size, page_size);

            // Enable stack fiber protection
            fiber_stack_protection(&fiber, true);

            // Retrieve the protection flags of the memory page
            protection_flags = fiber_stack_protection_test_find_memory_protection(
                    fiber.stack_base, (char*)fiber.stack_base + page_size);

            REQUIRE(protection_flags == PROT_NONE);

            mprotect(fiber.stack_base, page_size, PROT_READ | PROT_WRITE);
            free(fiber.stack_base);
        }

        SECTION("test disabling the memory protection") {
            int protection_flags = -1;
            fiber_t fiber = { NULL };
            fiber.stack_base = aligned_alloc(page_size, page_size);

            // Enable and disable the stack protection
            fiber_stack_protection(&fiber, true);
            fiber_stack_protection(&fiber, false);

            // Check if the page is listed in /proc/self/maps
            protection_flags = fiber_stack_protection_test_find_memory_protection(
                    fiber.stack_base, (char*)fiber.stack_base + page_size);

            // The protection flags for the allocated should be entirely missing as fiber_stack_protection resets them
            // to PROT_READ | PROT_WRITE that are the default ones.
            // This behaviour may change in the future as it's depends on the OS.
            REQUIRE(protection_flags == -1);

            free(fiber.stack_base);
        }
    }

    SECTION("fiber_new") {
        SECTION("allocate a new fiber") {
            int user_data = 0;

            fiber_t * fiber = fiber_new(stack_size, fiber_new_test_empty, &user_data);

            // Calculate the end of the stack to be 16 bytes aligned and with 128 bytes free for the red zone
            uintptr_t stack_pointer = (uintptr_t)fiber->stack_base + stack_size;
            stack_pointer &= -16L;
            stack_pointer -= 128;

            // Add room for the first push/pop
            stack_pointer -= sizeof(void*) * 1;

            REQUIRE(fiber);
            REQUIRE(fiber->stack_size == stack_size);
            REQUIRE(fiber->start_fp == fiber_new_test_empty);
            REQUIRE(fiber->start_fp_user_data == &user_data);
            REQUIRE((uintptr_t)fiber->stack_pointer == stack_pointer);

            // The fiber memory is protected, the memory protection has to be disabled before freeing the memory
            mprotect(fiber->stack_base, page_size, PROT_READ | PROT_WRITE);
            free(fiber->stack_base);
            free(fiber);
        }

        SECTION("fail to allocate a new fiber without stack") {
            fiber_t *fiber = fiber_new(0, NULL, NULL);
            REQUIRE(fiber == NULL);
        }

        SECTION("try to change non protected memory") {
            fiber_t *fiber = fiber_new(stack_size, NULL, NULL);
            *(uint64_t*)fiber->stack_pointer = 0;
            *(uint64_t*)((char*)fiber->stack_base + page_size) = 0;

            // The fiber memory is protected, the memory protection has to be disabled before freeing the memory
            mprotect(fiber->stack_base, page_size, PROT_READ | PROT_WRITE);
            free(fiber->stack_base);
            free(fiber);
        }
    }

    SECTION("fiber_free") {
        // There is no REQUIRE for this test, fiber_free is run to ensure it's not triggering a failure (if mprotect
        // fails the system will throw an error for trying to access a memory protected area)
        SECTION("allocate a new fiber") {
            fiber_t *fiber = fiber_new(stack_size, NULL, NULL);
            fiber_free(fiber);
        }
    }

    SECTION("fiber_context_get") {
        SECTION("get fiber context from executing function") {
            fiber_t *fiber = fiber_new(stack_size, NULL, NULL);

            void* fiber_context_get_test_sp_first_var = fiber_context_get_test(fiber);

            // The code below calculates the position of the address saved in the RSP register when the context was read,
            // the magic value 28 only works Linux x64 platforms as it is dependent on the ABI which changes between
            // different OSes and architectures
            REQUIRE((char*)fiber_context_get_test_sp_first_var - 28 == fiber->context.rsp);

            fiber_free(fiber);
        }
    }

    SECTION("fiber_context_swap") {
        SECTION("test fiber context swap") {
            uint64_t user_data_current = 0;
            uint64_t user_data_to = 0;
            fiber_t *fiber_current = fiber_new(
                    stack_size,
                    NULL,
                    &user_data_current);
            fiber_t *fiber_to = fiber_new(
                    stack_size,
                    fiber_context_swap_test_update_user_data_and_swap_back,
                    &user_data_to);

            // Swap from the current context to the newly allocated one, the function associated with the fiber
            // fiber_context_swap_test_update_user_data_and_swap_back, will take care of switching back after having
            // updated the user data for both the fibers to be able to test the proper switching
            fiber_context_swap(fiber_current, fiber_to);

            REQUIRE(*(uint64_t*)fiber_current->start_fp_user_data == 1);
            REQUIRE(*(uint64_t*)fiber_to->start_fp_user_data == 2);

            fiber_free(fiber_current);
            fiber_free(fiber_to);
        }
    }
}
