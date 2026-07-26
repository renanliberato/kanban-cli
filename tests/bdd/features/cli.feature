Feature: Non-interactive CLI subcommands
  Background:
    Given an empty board file at the default path

  Scenario: Add a card via CLI
    When I run "add" with arguments "test card"
    Then the output should contain a numeric ID
    And the "To Do" column should contain "test card"

  Scenario: Add a card with column flag
    When I run "add" with arguments "doing task" "--col" "doing"
    Then the "Doing" column should contain "doing task"

  Scenario: Add a card with description
    When I run "add" with arguments "task with desc" "--desc" "A description"
    And I run "show" with the last card id
    Then the output should contain "A description"

  Scenario: Add a card with AI enrichment
    When I run "add" with arguments "enrich me" "--ai"
    Then the output should contain a numeric ID
    And the card should have a description from AI

  Scenario: List all cards
    Given I have added cards "alpha", "beta" in "To Do" and "gamma" in "Doing"
    When I run "list" with no arguments
    Then the output should contain "alpha"
    And the output should contain "beta"
    And the output should contain "gamma"

  Scenario: List cards filtered by column
    Given I have added cards "alpha", "beta" in "To Do" and "gamma" in "Doing"
    When I run "list" with arguments "--col" "doing"
    Then the output should contain "gamma"
    And the output should not contain "alpha"

  Scenario: Show card details
    Given I have added card "show me" in "To Do"
    When I run "show" with the last card id
    Then the output should contain "show me"
    And the output should contain "Title"
    And the output should contain "Description"

  Scenario: Enrich an existing card via CLI
    Given I have added card "enrich via cli" in "To Do"
    When I run "enrich" with the last card id
    Then the output should contain "description"
    And the exit code should be 0

  Scenario: Move a card via CLI
    Given I have added card "movable" in "To Do"
    When I run "move" with the last card id and "doing"
    Then the "Doing" column should contain "movable"

  Scenario: CLI shows error for unknown card
    When I run "show" with arguments "99999"
    Then the output should contain "not found"
    And the exit code should be 1

  Scenario: CLI shows usage for bad subcommand
    When I run "move" with arguments "1"
    Then the output should contain "Usage"
    And the exit code should be 1
